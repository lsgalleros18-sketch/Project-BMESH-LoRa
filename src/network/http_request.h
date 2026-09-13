#pragma once

#include <stddef.h>

#include "esp_err.h"
#include "esp_http_server.h"

esp_err_t http_read_body_checked(httpd_req_t *req, char *buffer, size_t buffer_size);
