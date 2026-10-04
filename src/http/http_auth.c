#include "http/http_auth.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "esp_random.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "bems_common.h"
#include "bems_crypto.h"
#include "network/http_request.h"
#include "utils/string_utils.h"

#define SESSION_COOKIE_NAME "BMESH_SESSION"
#define SESSION_TOKEN_LEN 17
#define SESSION_IDLE_TIMEOUT_MS 900000

static http_auth_context_t auth_context;
static char session_token[SESSION_TOKEN_LEN];
static TickType_t session_last_activity_tick;
static TickType_t last_login_attempt_tick;
static uint8_t failed_login_count;

static bool cookie_has_session(const char *cookie)
{
    const size_t name_len = sizeof(SESSION_COOKIE_NAME) - 1;
    const char *cursor = cookie;
    while ((cursor = strstr(cursor, SESSION_COOKIE_NAME)) != NULL) {
        if ((cursor == cookie || cursor[-1] == ';' || cursor[-1] == ' ') &&
            strncmp(cursor + name_len, "=", 1) == 0 &&
            strncmp(cursor + name_len + 1, session_token, SESSION_TOKEN_LEN - 1) == 0 &&
            (cursor[name_len + SESSION_TOKEN_LEN] == ';' || cursor[name_len + SESSION_TOKEN_LEN] == '\0' || cursor[name_len + SESSION_TOKEN_LEN] == ' ')) return true;
        cursor += name_len;
    }
    return false;
}

static esp_err_t send_session_expired(httpd_req_t *request)
{
    httpd_resp_set_status(request, "401 Unauthorized");
    httpd_resp_set_type(request, "application/json");
    httpd_resp_set_hdr(request, "Cache-Control", "no-store");
    return httpd_resp_send(request, "{\"error\":\"session expired, please reload\"}", HTTPD_RESP_USE_STRLEN);
}

void http_auth_init(const http_auth_context_t *context)
{
    uint32_t random_a = esp_random();
    uint32_t random_b = esp_random();

    auth_context = *context;
    snprintf(session_token, sizeof(session_token), "%08lX%08lX", (unsigned long)random_a, (unsigned long)random_b);
    session_last_activity_tick = xTaskGetTickCount();
}

bool http_auth_request_has_session(httpd_req_t *request)
{
    char cookie[128] = {0};
    TickType_t now;

    if (!*auth_context.configured) {
        return true;
    }

    if (httpd_req_get_hdr_value_str(request, "Cookie", cookie, sizeof(cookie)) != ESP_OK) {
        return false;
    }
    if (strlen(cookie) >= sizeof(cookie) - 1) return false;

    if (!cookie_has_session(cookie)) {
        return false;
    }

    now = xTaskGetTickCount();
    if ((now - session_last_activity_tick) > pdMS_TO_TICKS(SESSION_IDLE_TIMEOUT_MS)) {
        return false;
    }

    session_last_activity_tick = now;
    return true;
}

esp_err_t http_auth_send_redirect(httpd_req_t *request, const char *location)
{
    httpd_resp_set_status(request, "302 Found");
    httpd_resp_set_hdr(request, "Location", location);
    httpd_resp_send(request, "", HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

esp_err_t http_auth_require_session(httpd_req_t *request)
{
    if (http_auth_request_has_session(request)) {
        return ESP_OK;
    }

    if (strncmp(request->uri, "/api/", 5) == 0 || request->method == HTTP_POST) {
        return send_session_expired(request);
    }

    return http_auth_send_redirect(request, "/");
}

esp_err_t http_auth_login_handler(httpd_req_t *request)
{
    char body[96] = {0};
    char pin[FIELD_LEN] = {0};
    char cookie[80];
    TickType_t now = xTaskGetTickCount();
    uint8_t backoff_seconds;

    if (!*auth_context.configured) return http_auth_send_redirect(request, "/setup");

    // Check brute-force protection
    if (failed_login_count > 0) {
        backoff_seconds = MIN(failed_login_count, 6) * (1u << (MIN(failed_login_count, 4) - 1));
        if ((now - last_login_attempt_tick) < pdMS_TO_TICKS(backoff_seconds * 1000)) {
            httpd_resp_set_status(request, "429 Too Many Requests");
            httpd_resp_set_type(request, "text/html");
            return httpd_resp_send(request, "<!doctype html><html><body><h2>Slow down.</h2><p>Too many login attempts.</p></body></html>", HTTPD_RESP_USE_STRLEN);
        }
    }
    last_login_attempt_tick = now;

    if (http_read_body_checked(request, body, sizeof(body)) != ESP_OK) {
        return ESP_FAIL;
    }

    form_value(body, "pin", pin, sizeof(pin));
    if (auth_context.web_pin == NULL || auth_context.web_pin[0] == '\0' ||
        !constant_time_equal((const uint8_t *)pin, (const uint8_t *)auth_context.web_pin, sizeof(pin))) {
        failed_login_count++;
        httpd_resp_set_status(request, "403 Forbidden");
        return auth_context.send_portal_file(request, "login.html");
    }

    failed_login_count = 0;
    snprintf(cookie, sizeof(cookie), "%s=%s; Path=/; HttpOnly; SameSite=Lax", SESSION_COOKIE_NAME, session_token);
    httpd_resp_set_hdr(request, "Set-Cookie", cookie);
    session_last_activity_tick = xTaskGetTickCount();
    return http_auth_send_redirect(request, "/");
}

esp_err_t http_auth_logout_handler(httpd_req_t *request)
{
    if (!*auth_context.configured) return http_auth_send_redirect(request, "/setup");
    uint32_t a = esp_random();
    uint32_t b = esp_random();
    snprintf(session_token, sizeof(session_token), "%08lX%08lX", (unsigned long)a, (unsigned long)b);
    session_last_activity_tick = 0;
    httpd_resp_set_hdr(request, "Set-Cookie", "BMESH_SESSION=; Path=/; Max-Age=0; HttpOnly; SameSite=Lax");
    return http_auth_send_redirect(request, "/login-page");
}
