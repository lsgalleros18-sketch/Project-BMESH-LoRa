#pragma once

#include <stdint.h>

#include "esp_err.h"
#include "esp_http_server.h"

typedef void (*http_time_apply_sync_fn)(uint32_t epoch, uint8_t distance);
typedef void (*http_time_send_sync_fn)(uint32_t epoch, uint8_t distance, uint8_t hops);

typedef struct {
    http_time_apply_sync_fn apply_time_sync;
    http_time_send_sync_fn send_time_sync_packet;
} http_time_context_t;

esp_err_t http_time_handler(httpd_req_t *request);
