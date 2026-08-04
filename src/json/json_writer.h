#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_http_server.h"
#include "bems_common.h"

#define PAYLOAD_LEN 140
#define PACKET_LEN 320

typedef struct {
    uint32_t id;
    char direction[8];
    char source[FIELD_LEN];
    char destination[FIELD_LEN];
    char type[FIELD_LEN];
    char priority[FIELD_LEN];
    char payload[PAYLOAD_LEN];
    char packet[PACKET_LEN];
    location_info_t origin_location;
    char thread_key[FIELD_LEN];
    char status[FIELD_LEN];
    uint32_t stored_epoch;
    int rssi;
    int snr;
    int hops;
} emergency_message_t;

void write_message_json_chunk(httpd_req_t *request, const emergency_message_t *message, bool first);
