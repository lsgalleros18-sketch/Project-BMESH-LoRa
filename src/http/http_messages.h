#pragma once

#include "esp_err.h"
#include "esp_http_server.h"

typedef esp_err_t (*http_messages_require_session_fn)(httpd_req_t *request);

typedef struct {
    http_messages_require_session_fn require_session;
} http_messages_context_t;

esp_err_t http_messages_handler(httpd_req_t *request);
