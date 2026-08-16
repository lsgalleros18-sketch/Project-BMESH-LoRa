#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#include "freertos/FreeRTOS.h"
#include "bems_common.h"
#define PAYLOAD_LEN 140
#define PACKET_LEN 320
#define MAX_SEEN_PACKETS 256
#define SEEN_PACKET_TTL_MS 60000
#define MAX_SEEN_BUCKETS 64

typedef struct {
    bool valid;
    uint32_t id;
    int hops;
    bool broadcast_destination;
    char source[FIELD_LEN];
    char destination[FIELD_LEN];
    char type[FIELD_LEN];
    char priority[FIELD_LEN];
    char relay[FIELD_LEN];
    char next_hop[FIELD_LEN];
    char location_raw[PACKET_LEN];
    location_info_t location;
    char thread_key[FIELD_LEN];
    char payload[PAYLOAD_LEN];
    size_t payload_len;
} mesh_packet_t;

typedef enum {
    BEMS_PACKET_FORMAT_V1 = 1,
    BEMS_PACKET_FORMAT_V2 = 2,
} bems_packet_format_t;

typedef enum {
    BEMS_PACKET_TYPE_FLOOD = 1,
    BEMS_PACKET_TYPE_ACK = 2,
    BEMS_PACKET_TYPE_TIME_SYNC = 3,
    BEMS_PACKET_TYPE_SYNC_REQ = 4,
    BEMS_PACKET_TYPE_SYNC_RESP = 5,
    BEMS_PACKET_TYPE_EMERGENCY = 6,
} bems_packet_type_t;

typedef enum {
    BEMS_PRIORITY_LOW = 0,
    BEMS_PRIORITY_NORMAL = 1,
    BEMS_PRIORITY_HIGH = 2,
} bems_priority_t;

// Parses a mesh packet string into structured data
bool parse_mesh_packet(const char *packet, mesh_packet_t *parsed);

// Parses a compact V2 packet buffer into structured data
bool parse_mesh_packet_v2(const uint8_t *packet, size_t packet_len, mesh_packet_t *parsed);

// Builds a compact V2 packet buffer from message data
bool build_forward_packet_v2(const mesh_packet_t *parsed, uint8_t *packet, size_t packet_size, size_t *packet_len);

// Returns true when the packet uses the compact V2 prefix
bool mesh_packet_is_v2(const uint8_t *packet, size_t packet_len);

// Returns the maximum plaintext bytes allowed for V2 packets.
size_t mesh_packet_v2_max_plaintext(void);

// Encodes location into tilde-separated string
void location_encode(const location_info_t *loc, char *out, size_t out_size);

// Decodes tilde-separated string into location
void location_decode(const char *encoded, location_info_t *loc);

// Checks if packet has been seen before (duplicate detection)
bool packet_seen(const char *source, uint32_t id);

// Consumes one hop from the packet and returns true if more forwarding is allowed.
bool mesh_packet_consume_hop(mesh_packet_t *packet);

// Remembers a packet as seen
void remember_packet(const char *source, uint32_t id);
void deduplication_debug_reset_for_test(void);
void deduplication_debug_set_seen_tick_for_test(const char *source, uint32_t id, TickType_t seen_tick);
