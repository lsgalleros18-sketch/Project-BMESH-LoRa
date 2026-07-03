#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#define FIELD_LEN 32
#define SITIO_LEN 24
#define BARANGAY_LEN 24
#define MUNICIPALITY_LEN 24
#define PAYLOAD_LEN 140
#define PACKET_LEN 320
#define MAX_SEEN_PACKETS 64
#define SEEN_PACKET_TTL_MS 60000

typedef struct {
    char sitio[SITIO_LEN];
    char barangay[BARANGAY_LEN];
    char municipality[MUNICIPALITY_LEN];
} location_info_t;

typedef struct {
    bool valid;
    uint32_t id;
    int hops;
    char source[FIELD_LEN];
    char destination[FIELD_LEN];
    char type[FIELD_LEN];
    char priority[FIELD_LEN];
    char relay[FIELD_LEN];
    char location_raw[PACKET_LEN];
    location_info_t location;
    char payload[PAYLOAD_LEN];
} mesh_packet_t;

// Parses a mesh packet string into structured data
bool parse_mesh_packet(const char *packet, mesh_packet_t *parsed);

// Builds packet string from message data
void build_forward_packet(const mesh_packet_t *parsed, char *packet, size_t packet_size);

// Encodes location into tilde-separated string
void location_encode(const location_info_t *loc, char *out, size_t out_size);

// Decodes tilde-separated string into location
void location_decode(const char *encoded, location_info_t *loc);

// Checks if packet has been seen before (duplicate detection)
bool packet_seen(const char *source, uint32_t id);

// Remembers a packet as seen
void remember_packet(const char *source, uint32_t id);
