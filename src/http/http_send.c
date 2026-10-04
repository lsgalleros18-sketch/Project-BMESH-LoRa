#include "http/http_send.h"

#include <string.h>
#include <ctype.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "bems_common.h"
#include "http/http_auth.h"
#include "messages/message_store.h"
#include "mesh_protocol.h"
#include "network/http_request.h"
#include "utils/string_utils.h"

static TickType_t last_send_tick;

static bool valid_node_destination(const char *destination)
{
    if (strcmp(destination, "ALL") == 0) return true;
    size_t length = strlen(destination);
    if (length < 2 || length >= FIELD_LEN) return false;
    for (size_t i = 0; i < length; i++) if (!isalnum((unsigned char)destination[i]) && destination[i] != '-' && destination[i] != '_') return false;
    return true;
}

esp_err_t http_send_handler(httpd_req_t *request)
{
    const http_send_context_t *context = request->user_ctx;
    char body[384] = {0};
    char destination[FIELD_LEN] = {0};
    char sender_name[FIELD_LEN] = {0};
    char type[FIELD_LEN] = {0};
    char priority[FIELD_LEN] = {0};
    char payload[PAYLOAD_LEN] = {0};
    esp_err_t session_result = http_auth_require_session(request);

    if (session_result != ESP_OK) {
        return session_result;
    }

    if (http_read_body_checked(request, body, sizeof(body)) != ESP_OK) {
        return ESP_FAIL;
    }

    form_value(body, "sender_name", sender_name, sizeof(sender_name));
    bool has_destination = form_value(body, "destination", destination, sizeof(destination));
    if (!has_destination || !valid_node_destination(destination)) {
        httpd_resp_set_status(request, "400 Bad Request");
        return httpd_resp_send(request, "Invalid destination", HTTPD_RESP_USE_STRLEN);
    }
    form_value(body, "type", type, sizeof(type));
    copy_field_no_delims(type, sizeof(type), type);
    form_value(body, "priority", priority, sizeof(priority));
    copy_field_no_delims(priority, sizeof(priority), priority);
    form_value(body, "payload", payload, sizeof(payload));
    if ((strcmp(type, "EMERGENCY") != 0 && strcmp(type, "FLOOD") != 0 && strcmp(type, "MEDICAL") != 0 && strcmp(type, "INFO") != 0) ||
        (strcmp(priority, "HIGH") != 0 && strcmp(priority, "NORMAL") != 0 && strcmp(priority, "LOW") != 0) || payload[0] == '\0') {
        httpd_resp_set_status(request, "400 Bad Request");
        return httpd_resp_send(request, "Message type, priority, and payload are required", HTTPD_RESP_USE_STRLEN);
    }
    if (strlen(payload) >= PAYLOAD_LEN || (sender_name[0] != '\0' && strlen(payload) + strlen(sender_name) + 2 >= PAYLOAD_LEN)) {
        httpd_resp_set_status(request, "400 Bad Request");
        return httpd_resp_send(request, "Message is too long", HTTPD_RESP_USE_STRLEN);
    }

    if ((xTaskGetTickCount() - last_send_tick) < pdMS_TO_TICKS(10000)) {
        httpd_resp_set_status(request, "429 Too Many Requests");
        httpd_resp_set_type(request, "text/html");
        return httpd_resp_send(request, "<!doctype html><html><body><h2>Slow down.</h2><p>Please wait before sending again.</p></body></html>", HTTPD_RESP_USE_STRLEN);
    }

    last_send_tick = xTaskGetTickCount();
    if (sender_name[0] != '\0') {
        char named_payload[PAYLOAD_LEN];
        size_t prefix_len;

        named_payload[0] = '\0';
        copy_field(named_payload, sizeof(named_payload), sender_name);
        prefix_len = strlen(named_payload);
        if (prefix_len < sizeof(named_payload) - 3) {
            named_payload[prefix_len++] = ':';
            named_payload[prefix_len++] = ' ';
            named_payload[prefix_len] = '\0';
            copy_field(named_payload + prefix_len, sizeof(named_payload) - prefix_len, payload);
        }
        copy_field(payload, sizeof(payload), named_payload);
    }

    context->queue_message(destination, type, priority, payload);
    return http_auth_send_redirect(request, "/");
}
