#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_http_server.h"
#include "messages/message_store.h"

void write_message_json_chunk(httpd_req_t *request, const emergency_message_t *message, bool first);
