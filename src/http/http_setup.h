#pragma once

#include <stddef.h>

#include "esp_err.h"
#include "esp_http_server.h"
#include "node_config.h"

typedef void (*http_setup_copy_node_id_fn)(char *destination, size_t destination_size, const char *source);
typedef esp_err_t (*http_setup_save_config_fn)(const node_config_t *config);

typedef struct {
    const char *node_id;
    const char *ap_password;
    const char *default_web_pin;
    const char *default_network_key;
    http_setup_copy_node_id_fn copy_node_id;
    http_setup_save_config_fn save_node_config;
} http_setup_context_t;

esp_err_t http_setup_handler(httpd_req_t *request);
