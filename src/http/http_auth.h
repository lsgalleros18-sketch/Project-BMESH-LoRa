#pragma once

#include <stdbool.h>

#include "esp_err.h"
#include "esp_http_server.h"

typedef esp_err_t (*http_auth_send_portal_file_fn)(httpd_req_t *request, const char *filename);

typedef struct {
    const bool *configured;
    const char *web_pin;
    http_auth_send_portal_file_fn send_portal_file;
} http_auth_context_t;

void http_auth_init(const http_auth_context_t *context);
bool http_auth_request_has_session(httpd_req_t *request);
esp_err_t http_auth_require_session(httpd_req_t *request);
esp_err_t http_auth_send_redirect(httpd_req_t *request, const char *location);
esp_err_t http_auth_login_handler(httpd_req_t *request);
