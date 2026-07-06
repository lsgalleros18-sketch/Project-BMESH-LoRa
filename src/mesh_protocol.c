#include "mesh_protocol.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

typedef struct {
    uint32_t id;
    TickType_t seen_tick;
    char source[FIELD_LEN];
} seen_packet_t;

static SemaphoreHandle_t mesh_mutex;
static seen_packet_t seen_packets[MAX_SEEN_PACKETS];
static size_t seen_packet_count;

static void ensure_mutex(void)
{
    if (mesh_mutex == NULL) {
        mesh_mutex = xSemaphoreCreateMutex();
    }
}

static void mesh_lock(void)
{
    if (mesh_mutex != NULL) {
        xSemaphoreTake(mesh_mutex, portMAX_DELAY);
    }
}

static void mesh_unlock(void)
{
    if (mesh_mutex != NULL) {
        xSemaphoreGive(mesh_mutex);
    }
}

static void copy_field(char *destination, size_t destination_size, const char *source)
{
    size_t write_index = 0;

    if (destination_size == 0) {
        return;
    }

    while (*source != '\0' && write_index < destination_size - 1) {
        unsigned char character = (unsigned char)*source++;
        if (character >= 32 && character <= 126) {
            destination[write_index++] = (char)character;
        }
    }

    destination[write_index] = '\0';
}

void location_encode(const location_info_t *loc, char *out, size_t out_size)
{
    if (out_size == 0) {
        return;
    }

    snprintf(out, out_size, "%.*s~%.*s~%.*s",
             SITIO_LEN - 1, loc->sitio,
             BARANGAY_LEN - 1, loc->barangay,
             MUNICIPALITY_LEN - 1, loc->municipality);
}

void location_decode(const char *encoded, location_info_t *loc)
{
    char buffer[SITIO_LEN + BARANGAY_LEN + MUNICIPALITY_LEN + 2];
    char *first_sep;
    char *second_sep;

    memset(loc, 0, sizeof(*loc));
    if (encoded == NULL) {
        return;
    }

    copy_field(buffer, sizeof(buffer), encoded);
    first_sep = strchr(buffer, '~');
    if (first_sep == NULL) {
        copy_field(loc->barangay, sizeof(loc->barangay), buffer);
        return;
    }

    *first_sep = '\0';
    copy_field(loc->sitio, sizeof(loc->sitio), buffer);
    second_sep = strchr(first_sep + 1, '~');
    if (second_sep == NULL) {
        copy_field(loc->barangay, sizeof(loc->barangay), first_sep + 1);
        return;
    }

    *second_sep = '\0';
    copy_field(loc->barangay, sizeof(loc->barangay), first_sep + 1);
    copy_field(loc->municipality, sizeof(loc->municipality), second_sep + 1);
}

bool parse_mesh_packet(const char *packet, mesh_packet_t *parsed)
{
    char packet_copy[PACKET_LEN];
    char *fields[10] = {0};
    char *cursor = packet_copy;
    size_t field_count = 0;

    memset(parsed, 0, sizeof(*parsed));
    copy_field(packet_copy, sizeof(packet_copy), packet);

    while (field_count < sizeof(fields) / sizeof(fields[0]) && cursor != NULL) {
        fields[field_count++] = cursor;
        if (field_count == sizeof(fields) / sizeof(fields[0])) {
            break;
        }

        cursor = strchr(cursor, '|');
        if (cursor != NULL) {
            *cursor = '\0';
            cursor++;
        }
    }

    if (field_count < 10 || strcmp(fields[0], "BEMS") != 0) {
        return false;
    }

    parsed->valid = true;
    parsed->id = (uint32_t)strtoul(fields[1], NULL, 10);
    parsed->hops = strncmp(fields[6], "HOPS=", 5) == 0 ? atoi(fields[6] + 5) : 0;
    copy_field(parsed->source, sizeof(parsed->source), fields[2]);
    copy_field(parsed->destination, sizeof(parsed->destination), fields[3]);
    copy_field(parsed->type, sizeof(parsed->type), fields[4]);
    copy_field(parsed->priority, sizeof(parsed->priority), fields[5]);
    copy_field(parsed->relay, sizeof(parsed->relay), fields[7]);
    copy_field(parsed->location_raw, sizeof(parsed->location_raw), fields[8]);
    location_decode(fields[8], &parsed->location);
    copy_field(parsed->payload, sizeof(parsed->payload), fields[9]);
    return true;
}

void build_forward_packet(const mesh_packet_t *parsed, char *packet, size_t packet_size)
{
    int next_hops = MAX(parsed->hops - 1, 0);
    char encoded_location[SITIO_LEN + BARANGAY_LEN + MUNICIPALITY_LEN + 2];

    location_encode(&parsed->location, encoded_location, sizeof(encoded_location));
    snprintf(packet, packet_size, "BEMS|%lu|%.*s|%.*s|%.*s|%.*s|HOPS=%d|%.*s|%s|%.*s",
             (unsigned long)parsed->id,
             31,
             parsed->source,
             31,
             parsed->destination,
             31,
             parsed->type,
             31,
             parsed->priority,
             next_hops,
             31,
             parsed->relay,
             encoded_location,
             120,
             parsed->payload);
}

bool packet_seen(const char *source, uint32_t id)
{
    bool seen = false;
    TickType_t now = xTaskGetTickCount();
    TickType_t ttl_ticks = pdMS_TO_TICKS(SEEN_PACKET_TTL_MS);

    ensure_mutex();
    mesh_lock();
    for (size_t i = 0; i < seen_packet_count;) {
        if ((now - seen_packets[i].seen_tick) > ttl_ticks) {
            seen_packets[i] = seen_packets[seen_packet_count - 1];
            seen_packet_count--;
            continue;
        }

        if (seen_packets[i].id == id && strcmp(seen_packets[i].source, source) == 0) {
            seen = true;
            break;
        }

        i++;
    }
    mesh_unlock();
    return seen;
}

void remember_packet(const char *source, uint32_t id)
{
    seen_packet_t *seen_packet;
    TickType_t now = xTaskGetTickCount();
    TickType_t ttl_ticks = pdMS_TO_TICKS(SEEN_PACKET_TTL_MS);
    size_t slot_index = 0;

    ensure_mutex();
    mesh_lock();
    for (size_t i = 0; i < seen_packet_count;) {
        if ((now - seen_packets[i].seen_tick) > ttl_ticks) {
            seen_packets[i] = seen_packets[seen_packet_count - 1];
            seen_packet_count--;
            continue;
        }

        i++;
    }

    if (seen_packet_count < MAX_SEEN_PACKETS) {
        seen_packet = &seen_packets[seen_packet_count++];
    } else {
        for (size_t i = 1; i < seen_packet_count; i++) {
            if ((now - seen_packets[i].seen_tick) > (now - seen_packets[slot_index].seen_tick)) {
                slot_index = i;
            }
        }
        seen_packet = &seen_packets[slot_index];
    }

    seen_packet->id = id;
    seen_packet->seen_tick = now;
    copy_field(seen_packet->source, sizeof(seen_packet->source), source);
    mesh_unlock();
}
