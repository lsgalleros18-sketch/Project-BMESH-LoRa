#include "http/http_setup.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "http/http_auth.h"
#include "http/http_portal.h"
#include "network/http_request.h"
#include "utils/string_utils.h"

static bool form_contains(const char *body, const char *key)
{
    char marker[FIELD_LEN + 2];
    int length = snprintf(marker, sizeof(marker), "%s=", key);
    if (length <= 0 || (size_t)length >= sizeof(marker)) return false;
    const char *cursor = body;
    while ((cursor = strstr(cursor, marker)) != NULL) {
        if (cursor == body || cursor[-1] == '&') return true;
        cursor += (size_t)length;
    }
    return false;
}

static bool valid_node_id(const char *value)
{
    size_t length = strlen(value);
    if (length < 2 || length >= FIELD_LEN) return false;
    for (size_t i = 0; i < length; i++) if (!isalnum((unsigned char)value[i]) && value[i] != '-' && value[i] != '_') return false;
    return true;
}

static bool valid_destination(const char *value)
{
    return strcmp(value, "ALL") == 0 || valid_node_id(value);
}

static bool valid_delimited_field(const char *value)
{
    for (; *value; value++) if ((unsigned char)*value < 0x20 || *value == '|' || *value == '~') return false;
    return true;
}

static esp_err_t invalid_setup(httpd_req_t *request, const char *reason)
{
    httpd_resp_set_status(request, "400 Bad Request");
    httpd_resp_set_type(request, "text/plain");
    return httpd_resp_send(request, reason, HTTPD_RESP_USE_STRLEN);
}

esp_err_t http_setup_get_handler(httpd_req_t *request)
{
    const http_setup_context_t *context = request->user_ctx;
    if (*context->configured && !http_auth_request_has_session(request)) return http_auth_send_redirect(request, "/login-page");
    if (*context->configured) {
        httpd_resp_set_type(request, "text/html");
        return httpd_resp_send(request, "<!doctype html><html><body><h1>Node settings</h1><p>Settings are protected. Use the dashboard to monitor this node.</p><a href='/'>Return to dashboard</a><form method='post' action='/logout'><button>Log out</button></form></body></html>", HTTPD_RESP_USE_STRLEN);
    }
    return http_portal_send_file(request, "setup.html");
}

esp_err_t http_setup_handler(httpd_req_t *request)
{
    const http_setup_context_t *context = request->user_ctx;
    char body[384] = {0};
    char raw_node_id[FIELD_LEN] = {0};
    node_config_t config = {0};

    if (*context->configured) {
        esp_err_t auth = context->require_session(request);
        if (auth != ESP_OK) return auth;
    }
    if (http_read_body_checked(request, body, sizeof(body)) != ESP_OK) return invalid_setup(request, "Invalid or oversized setup form");

    (void)form_value(body, "node_id", raw_node_id, sizeof(raw_node_id));
    context->copy_node_id(config.node_id, sizeof(config.node_id), raw_node_id);
    if (strlen(raw_node_id) != strlen(config.node_id)) return invalid_setup(request, "Node ID contains invalid characters");
    (void)form_value(body, "node_name", config.node_name, sizeof(config.node_name));
    (void)form_value(body, "node_role", config.node_role, sizeof(config.node_role));
    (void)form_value(body, "sitio", config.location.sitio, sizeof(config.location.sitio));
    (void)form_value(body, "barangay", config.location.barangay, sizeof(config.location.barangay));
    (void)form_value(body, "municipality", config.location.municipality, sizeof(config.location.municipality));
    (void)form_value(body, "default_destination", config.default_destination, sizeof(config.default_destination));
    (void)form_value(body, "default_priority", config.default_priority, sizeof(config.default_priority));
    (void)form_value(body, "ap_password", config.ap_password, sizeof(config.ap_password));
    (void)form_value(body, "web_pin", config.web_pin, sizeof(config.web_pin));
    (void)form_value(body, "duress_pin", config.duress_pin, sizeof(config.duress_pin));
    (void)form_value(body, "network_key", config.network_key, sizeof(config.network_key));

    const char *required[] = {"node_id", "node_name", "node_role", "default_destination", "default_priority", "ap_password", "web_pin", "network_key"};
    for (size_t i = 0; i < sizeof(required) / sizeof(required[0]); i++) if (!form_contains(body, required[i])) return invalid_setup(request, "A required configuration field is missing");
    if (!valid_node_id(config.node_id)) return invalid_setup(request, "Node ID must be 2-31 letters, digits, '-' or '_'");
    if (config.node_name[0] == '\0' || !valid_delimited_field(config.node_name)) return invalid_setup(request, "Node name is required and cannot contain protocol delimiters");
    if (strcmp(config.node_role, "relay-only") != 0 && strcmp(config.node_role, "household") != 0 && strcmp(config.node_role, "command-post") != 0) return invalid_setup(request, "Unsupported node role");
    if (!valid_delimited_field(config.location.sitio) || !valid_delimited_field(config.location.barangay) || !valid_delimited_field(config.location.municipality)) return invalid_setup(request, "Location contains unsupported characters");
    if (!valid_destination(config.default_destination)) return invalid_setup(request, "Default destination must be ALL or a valid Node ID");
    if (strcmp(config.default_priority, "HIGH") != 0 && strcmp(config.default_priority, "NORMAL") != 0 && strcmp(config.default_priority, "LOW") != 0) return invalid_setup(request, "Unsupported default priority");
    if (strlen(config.ap_password) < 8 || strcmp(config.ap_password, "123456789") == 0) return invalid_setup(request, "AP password must be 8-31 characters and not the default password");
    if (strlen(config.web_pin) < 8 || strcmp(config.web_pin, "123456789") == 0) return invalid_setup(request, "Web PIN must be at least 8 characters and not the default PIN");
    if (strlen(config.network_key) < 16 || strcmp(config.network_key, "CHANGEME1234567") == 0 || strcmp(config.network_key, "CHANGEME12345678") == 0) return invalid_setup(request, "Network key must be at least 16 characters and not the default key");
    if (config.duress_pin[0] != '\0' && strlen(config.duress_pin) < 8) return invalid_setup(request, "Duress PIN must be empty or at least 8 characters");

    config.configured = true;
    esp_err_t result = context->save_node_config(&config);
    if (result != ESP_OK) return result;
    (void)http_portal_send_file(request, "reboot.html");
    vTaskDelay(pdMS_TO_TICKS(500));
    esp_restart();
    return ESP_OK;
}
