#pragma once

#include <stddef.h>

#include "esp_err.h"
#include "esp_http_server.h"
#include "node_config.h"

typedef void (*http_setup_copy_node_id_fn)(char *destination, size_t destination_size, const char *source);
typedef esp_err_t (*http_setup_save_config_fn)(const node_config_t *config);
typedef esp_err_t (*http_setup_require_session_fn)(httpd_req_t *request);

typedef struct {
    http_setup_copy_node_id_fn copy_node_id;
    http_setup_save_config_fn save_node_config;
    http_setup_require_session_fn require_session;
    const bool *configured;
} http_setup_context_t;

esp_err_t http_setup_get_handler(httpd_req_t *request);
esp_err_t http_setup_handler(httpd_req_t *request);
