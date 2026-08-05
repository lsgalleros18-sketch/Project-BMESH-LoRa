#pragma once

#include "esp_err.h"
#include "esp_http_server.h"

typedef void (*http_sync_request_fn)(void);

typedef struct {
    http_sync_request_fn send_manual_sync_request;
} http_sync_context_t;

esp_err_t http_sync_handler(httpd_req_t *request);
