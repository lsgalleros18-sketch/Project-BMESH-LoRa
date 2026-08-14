#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

typedef struct {
    bool valid;
    size_t question_end;
    uint16_t qdcount;
    uint16_t qtype;
    uint16_t qclass;
} dns_request_info_t;

bool dns_server_parse_request(const uint8_t *packet, size_t length, dns_request_info_t *info);
bool dns_server_build_response(uint8_t *buffer, size_t buffer_size, const dns_request_info_t *request, uint32_t ap_ip, size_t *response_length);
esp_err_t dns_server_init(void);
