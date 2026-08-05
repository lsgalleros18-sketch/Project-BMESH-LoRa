#pragma once

#include "esp_err.h"
#include "esp_http_server.h"

typedef void (*http_reset_erase_config_fn)(void);

typedef struct {
    http_reset_erase_config_fn erase_node_config;
} http_reset_context_t;

esp_err_t http_reset_handler(httpd_req_t *request);
