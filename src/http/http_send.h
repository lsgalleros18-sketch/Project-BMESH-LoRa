#pragma once

#include "esp_err.h"
#include "esp_http_server.h"

typedef void (*http_send_queue_message_fn)(const char *destination, const char *type, const char *priority, const char *payload);

typedef struct {
    const char *default_destination;
    http_send_queue_message_fn queue_message;
} http_send_context_t;

esp_err_t http_send_handler(httpd_req_t *request);
