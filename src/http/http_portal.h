#pragma once

#include <stdbool.h>

#include "esp_err.h"
#include "esp_http_server.h"

typedef struct {
    const bool *configured;
    const bool *littlefs_mounted;
    const char *littlefs_base_path;
} http_portal_context_t;

void http_portal_init(const http_portal_context_t *context);
esp_err_t http_portal_send_file(httpd_req_t *request, const char *filename);
esp_err_t http_portal_index_handler(httpd_req_t *request);
esp_err_t http_portal_captive_handler(httpd_req_t *request);
