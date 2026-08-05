#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "esp_http_server.h"

typedef esp_err_t (*http_status_require_session_fn)(httpd_req_t *request);
typedef uint32_t (*http_status_current_epoch_fn)(void);

typedef struct {
    http_status_require_session_fn require_session;
    http_status_current_epoch_fn current_epoch_seconds;
    const char *node_id;
    const char *node_name;
    const char *node_role;
    const char *location;
    const char *ssid;
    const bool *configured;
    const bool *duplicate_node_id_warning;
    const bool *time_synced;
} http_status_context_t;

esp_err_t http_status_handler(httpd_req_t *request);
