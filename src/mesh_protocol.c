#include "mesh_protocol.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "bems_crypto.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

enum {
    DEDUP_EXPIRY_BUCKET_MS = 1000,
    DEDUP_EXPIRY_BUCKET_COUNT = SEEN_PACKET_TTL_MS / DEDUP_EXPIRY_BUCKET_MS,
    V2_MAGIC = 0xB2,
    V2_FLAG_NEXT_HOP = 0x01u,
    V2_FLAG_LOCATION = 0x02u,
    V2_BROADCAST_ID = 0xFFu,
};

typedef struct {
    uint32_t id;
    TickType_t seen_tick;
    char source[FIELD_LEN];
    int16_t next_hash;
    int16_t next_expiry;
    uint8_t expiry_bucket;
    bool active;
} seen_packet_t;

static SemaphoreHandle_t mesh_mutex;
static seen_packet_t seen_packets[MAX_SEEN_PACKETS];
static int16_t hash_buckets[MAX_SEEN_BUCKETS];
static int16_t expiry_wheel[DEDUP_EXPIRY_BUCKET_COUNT];
static int16_t free_list_head;
static uint32_t last_expiry_bucket;
static bool dedup_initialized;

static void ensure_mutex(void)
{
    if (mesh_mutex == NULL) {
        mesh_mutex = xSemaphoreCreateMutex();
    }
}

static uint32_t dedup_now_ms(void)
{
    return (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
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

static uint32_t seen_hash(const char *source, uint32_t id)
{
    uint32_t hash = 2166136261u;

    if (source == NULL) {
        source = "";
    }

    while (*source != '\0') {
        hash ^= (uint8_t)*source++;
        hash *= 16777619u;
    }

    hash ^= id;
    hash *= 16777619u;
    hash ^= id >> 16;
    hash *= 16777619u;
    return hash;
}

static int seen_bucket_index(const char *source, uint32_t id)
{
    return (int)(seen_hash(source, id) % MAX_SEEN_BUCKETS);
}

static bool seen_entry_matches(const seen_packet_t *entry, const char *source, uint32_t id)
{
    return entry->id == id && strcmp(entry->source, source) == 0;
}

static void copy_field(char *destination, size_t destination_size, const char *source)
{
    size_t write_index = 0;

    if (destination == NULL || destination_size == 0) {
        return;
    }

    if (source == NULL) {
        destination[0] = '\0';
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

static void update_thread_key(mesh_packet_t *parsed)
{
    if (parsed == NULL) {
        return;
    }
    if (parsed->broadcast_destination || parsed->destination[0] == '\0' || strcmp(parsed->destination, "ALL") == 0) {
        copy_field(parsed->thread_key, sizeof(parsed->thread_key), "ANNOUNCEMENTS");
    } else {
        copy_field(parsed->thread_key, sizeof(parsed->thread_key), parsed->source);
    }
}

static void remove_from_hash(int16_t index)
{
    int bucket_index;
    int16_t *cursor;

    if (index < 0 || index >= MAX_SEEN_PACKETS) {
        return;
    }

    bucket_index = seen_bucket_index(seen_packets[index].source, seen_packets[index].id);
    cursor = &hash_buckets[bucket_index];
    while (*cursor != -1) {
        if (*cursor == index) {
            *cursor = seen_packets[index].next_hash;
            break;
        }
        cursor = &seen_packets[*cursor].next_hash;
    }
}

static void remove_from_expiry(int16_t index)
{
    int16_t *cursor;
    uint8_t bucket;

    if (index < 0 || index >= MAX_SEEN_PACKETS) {
        return;
    }

    bucket = seen_packets[index].expiry_bucket;
    cursor = &expiry_wheel[bucket];
    while (*cursor != -1) {
        if (*cursor == index) {
            *cursor = seen_packets[index].next_expiry;
            break;
        }
        cursor = &seen_packets[*cursor].next_expiry;
    }
}

static void dedup_init_storage(void)
{
    if (dedup_initialized) {
        return;
    }

    for (size_t i = 0; i < MAX_SEEN_PACKETS; i++) {
        seen_packets[i].active = false;
        seen_packets[i].next_hash = -1;
        seen_packets[i].next_expiry = -1;
    }
    for (size_t i = 0; i < MAX_SEEN_BUCKETS; i++) {
        hash_buckets[i] = -1;
    }
    for (size_t i = 0; i < DEDUP_EXPIRY_BUCKET_COUNT; i++) {
        expiry_wheel[i] = -1;
    }
    free_list_head = 0;
    for (int16_t i = 0; i < MAX_SEEN_PACKETS - 1; i++) {
        seen_packets[i].next_hash = i + 1;
    }
    seen_packets[MAX_SEEN_PACKETS - 1].next_hash = -1;
    last_expiry_bucket = 0;
    dedup_initialized = true;
}

static void expire_current_and_prior_buckets(uint32_t now_ms)
{
    uint32_t current_bucket = now_ms / DEDUP_EXPIRY_BUCKET_MS;
    uint32_t bucket_span = current_bucket - last_expiry_bucket;

    if (bucket_span > DEDUP_EXPIRY_BUCKET_COUNT) {
        bucket_span = DEDUP_EXPIRY_BUCKET_COUNT;
    }

    for (uint32_t step = 0; step < bucket_span; step++) {
        uint32_t wheel_index = last_expiry_bucket % DEDUP_EXPIRY_BUCKET_COUNT;
        int16_t index = expiry_wheel[wheel_index];
        while (index != -1) {
            int16_t next = seen_packets[index].next_expiry;
            if (seen_packets[index].active &&
                (now_ms - (uint32_t)seen_packets[index].seen_tick) >= SEEN_PACKET_TTL_MS) {
                remove_from_hash(index);
                remove_from_expiry(index);
                seen_packets[index].active = false;
                seen_packets[index].next_hash = free_list_head;
                free_list_head = index;
            }
            index = next;
        }
        expiry_wheel[wheel_index] = -1;
        last_expiry_bucket++;
    }
}

static int16_t alloc_entry(void)
{
    int16_t index;

    if (free_list_head == -1) {
        return -1;
    }
    index = free_list_head;
    free_list_head = seen_packets[index].next_hash;
    return index;
}

static void insert_entry(int16_t index, const char *source, uint32_t id, TickType_t now_ticks)
{
    int bucket = seen_bucket_index(source, id);
    uint32_t bucket_id = (uint32_t)now_ticks / DEDUP_EXPIRY_BUCKET_MS;
    uint8_t expiry_bucket = (uint8_t)(bucket_id % DEDUP_EXPIRY_BUCKET_COUNT);

    seen_packets[index].id = id;
    seen_packets[index].seen_tick = now_ticks;
    copy_field(seen_packets[index].source, sizeof(seen_packets[index].source), source);
    seen_packets[index].expiry_bucket = expiry_bucket;
    seen_packets[index].active = true;

    seen_packets[index].next_hash = hash_buckets[bucket];
    hash_buckets[bucket] = index;

    seen_packets[index].next_expiry = expiry_wheel[expiry_bucket];
    expiry_wheel[expiry_bucket] = index;
}

static uint8_t type_to_u8(const char *type)
{
    if (type == NULL) return BEMS_PACKET_TYPE_FLOOD;
    if (strcmp(type, "ACK") == 0) return BEMS_PACKET_TYPE_ACK;
    if (strcmp(type, "TIME_SYNC") == 0) return BEMS_PACKET_TYPE_TIME_SYNC;
    if (strcmp(type, "SYNC_REQ") == 0) return BEMS_PACKET_TYPE_SYNC_REQ;
    if (strcmp(type, "SYNC_RESP") == 0) return BEMS_PACKET_TYPE_SYNC_RESP;
    if (strcmp(type, "EMERGENCY") == 0) return BEMS_PACKET_TYPE_EMERGENCY;
    return BEMS_PACKET_TYPE_FLOOD;
}

static const char *type_from_u8(uint8_t type)
{
    switch (type) {
    case BEMS_PACKET_TYPE_ACK: return "ACK";
    case BEMS_PACKET_TYPE_TIME_SYNC: return "TIME_SYNC";
    case BEMS_PACKET_TYPE_SYNC_REQ: return "SYNC_REQ";
    case BEMS_PACKET_TYPE_SYNC_RESP: return "SYNC_RESP";
    case BEMS_PACKET_TYPE_EMERGENCY: return "EMERGENCY";
    default: return "FLOOD";
    }
}

static uint8_t priority_to_u8(const char *priority)
{
    if (priority == NULL) return BEMS_PRIORITY_NORMAL;
    if (strcmp(priority, "HIGH") == 0) return BEMS_PRIORITY_HIGH;
    if (strcmp(priority, "LOW") == 0) return BEMS_PRIORITY_LOW;
    return BEMS_PRIORITY_NORMAL;
}

static const char *priority_from_u8(uint8_t priority)
{
    switch (priority) {
    case BEMS_PRIORITY_LOW: return "LOW";
    case BEMS_PRIORITY_HIGH: return "HIGH";
    default: return "NORMAL";
    }
}

static bool write_u8(uint8_t *packet, size_t packet_size, size_t *offset, uint8_t value)
{
    if (*offset >= packet_size) {
        return false;
    }
    packet[(*offset)++] = value;
    return true;
}

static bool write_u32(uint8_t *packet, size_t packet_size, size_t *offset, uint32_t value)
{
    if (*offset + 4 > packet_size) {
        return false;
    }
    packet[(*offset)++] = (uint8_t)(value & 0xFF);
    packet[(*offset)++] = (uint8_t)((value >> 8) & 0xFF);
    packet[(*offset)++] = (uint8_t)((value >> 16) & 0xFF);
    packet[(*offset)++] = (uint8_t)((value >> 24) & 0xFF);
    return true;
}

static bool write_bytes(uint8_t *packet, size_t packet_size, size_t *offset, const uint8_t *data, size_t len)
{
    if (*offset + len > packet_size) {
        return false;
    }
    if (len > 0) {
        memcpy(&packet[*offset], data, len);
        *offset += len;
    }
    return true;
}

static bool read_u8(const uint8_t *packet, size_t packet_len, size_t *offset, uint8_t *value)
{
    if (*offset >= packet_len) return false;
    *value = packet[(*offset)++];
    return true;
}

static bool read_u32(const uint8_t *packet, size_t packet_len, size_t *offset, uint32_t *value)
{
    if (*offset + 4 > packet_len) return false;
    *value = (uint32_t)packet[*offset] |
             ((uint32_t)packet[*offset + 1] << 8) |
             ((uint32_t)packet[*offset + 2] << 16) |
             ((uint32_t)packet[*offset + 3] << 24);
    *offset += 4;
    return true;
}

static bool read_bytes(const uint8_t *packet, size_t packet_len, size_t *offset, uint8_t *out, size_t out_size, size_t len)
{
    if (*offset + len > packet_len || len >= out_size) {
        return false;
    }
    if (len > 0) {
        memcpy(out, &packet[*offset], len);
        out[len] = '\0';
        *offset += len;
    } else if (out_size > 0) {
        out[0] = '\0';
    }
    return true;
}

void location_encode(const location_info_t *loc, char *out, size_t out_size)
{
    if (out_size == 0 || out == NULL || loc == NULL) {
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

    if (loc == NULL) {
        return;
    }

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
    char *fields[11] = {0};
    char *cursor = packet_copy;
    size_t field_count = 0;

    if (packet == NULL || parsed == NULL) {
        return false;
    }

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

    if ((field_count < 10 || field_count > 11) || strcmp(fields[0], "BEMS") != 0) {
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
    if (field_count == 11) {
        copy_field(parsed->next_hop, sizeof(parsed->next_hop), fields[8]);
        copy_field(parsed->location_raw, sizeof(parsed->location_raw), fields[9]);
        location_decode(fields[9], &parsed->location);
        copy_field(parsed->payload, sizeof(parsed->payload), fields[10]);
    } else {
        parsed->next_hop[0] = '\0';
        copy_field(parsed->location_raw, sizeof(parsed->location_raw), fields[8]);
        location_decode(fields[8], &parsed->location);
        copy_field(parsed->payload, sizeof(parsed->payload), fields[9]);
    }
    parsed->broadcast_destination = strcmp(parsed->destination, "ALL") == 0;
    parsed->payload_len = strnlen(parsed->payload, sizeof(parsed->payload));
    update_thread_key(parsed);
    return true;
}

bool mesh_packet_is_v2(const uint8_t *packet, size_t packet_len)
{
    return packet != NULL && packet_len >= 2 && packet[0] == V2_MAGIC && packet[1] == BEMS_PACKET_FORMAT_V2;
}

size_t mesh_packet_v2_max_plaintext(void)
{
    return BEMS_MAX_PLAINTEXT;
}

bool parse_mesh_packet_v2(const uint8_t *packet, size_t packet_len, mesh_packet_t *parsed)
{
    size_t offset = 0;
    uint8_t flags = 0;
    uint8_t type = 0;
    uint8_t priority = 0;
    uint8_t source_len = 0;
    uint8_t destination_len = 0;
    uint8_t relay_len = 0;
    uint8_t next_hop_len = 0;
    uint8_t hops = 0;
    uint8_t location_len = 0;
    uint8_t payload_len = 0;

    if (packet == NULL || parsed == NULL || !mesh_packet_is_v2(packet, packet_len)) {
        return false;
    }

    memset(parsed, 0, sizeof(*parsed));
    offset = 2;

    if (!read_u8(packet, packet_len, &offset, &flags) ||
        !read_u8(packet, packet_len, &offset, &type) ||
        !read_u8(packet, packet_len, &offset, &priority) ||
        !read_u8(packet, packet_len, &offset, &source_len) ||
        source_len == 0 ||
        !read_bytes(packet, packet_len, &offset, (uint8_t *)parsed->source, sizeof(parsed->source), source_len) ||
        !read_u8(packet, packet_len, &offset, &destination_len)) {
        return false;
    }

    if (destination_len == V2_BROADCAST_ID) {
        parsed->broadcast_destination = true;
        copy_field(parsed->destination, sizeof(parsed->destination), "ALL");
    } else {
        if (destination_len == 0 ||
            !read_bytes(packet, packet_len, &offset, (uint8_t *)parsed->destination, sizeof(parsed->destination), destination_len)) {
            return false;
        }
    }

    if (!read_u32(packet, packet_len, &offset, &parsed->id) ||
        !read_u8(packet, packet_len, &offset, &relay_len) ||
        relay_len == 0 ||
        !read_bytes(packet, packet_len, &offset, (uint8_t *)parsed->relay, sizeof(parsed->relay), relay_len) ||
        !read_u8(packet, packet_len, &offset, &hops)) {
        return false;
    }

    parsed->hops = (int)hops;
    if ((flags & V2_FLAG_NEXT_HOP) != 0u) {
        if (!read_u8(packet, packet_len, &offset, &next_hop_len) ||
            next_hop_len == 0 ||
            !read_bytes(packet, packet_len, &offset, (uint8_t *)parsed->next_hop, sizeof(parsed->next_hop), next_hop_len)) {
            return false;
        }
    }

    if ((flags & V2_FLAG_LOCATION) != 0u) {
        if (!read_u8(packet, packet_len, &offset, &location_len) ||
            (location_len > 0 && offset + location_len > packet_len)) {
            return false;
        }
        if (location_len > 0) {
            memcpy(parsed->location_raw, &packet[offset], location_len);
            parsed->location_raw[location_len] = '\0';
            location_decode(parsed->location_raw, &parsed->location);
            offset += location_len;
        }
    }

    if (!read_u8(packet, packet_len, &offset, &payload_len) ||
        offset + payload_len > packet_len ||
        payload_len >= sizeof(parsed->payload)) {
        return false;
    }

    if (payload_len > 0) {
        memcpy(parsed->payload, &packet[offset], payload_len);
        parsed->payload[payload_len] = '\0';
        offset += payload_len;
    }

    parsed->valid = true;
    parsed->type[0] = '\0';
    copy_field(parsed->type, sizeof(parsed->type), type_from_u8(type));
    copy_field(parsed->priority, sizeof(parsed->priority), priority_from_u8(priority));
    parsed->payload_len = payload_len;
    if (parsed->destination[0] == '\0') {
        copy_field(parsed->destination, sizeof(parsed->destination), "ALL");
        parsed->broadcast_destination = true;
    }
    update_thread_key(parsed);
    return true;
}

bool build_forward_packet_v2(const mesh_packet_t *parsed, uint8_t *packet, size_t packet_size, size_t *packet_len)
{
    char encoded_location[SITIO_LEN + BARANGAY_LEN + MUNICIPALITY_LEN + 2];
    size_t offset = 0;
    uint8_t flags = 0;
    uint8_t type;
    uint8_t priority;
    uint8_t source_len;
    uint8_t destination_len;
    uint8_t relay_len;
    uint8_t next_hop_len = 0;
    uint8_t location_len = 0;
    uint8_t payload_len;
    size_t required;

    if (parsed == NULL || packet == NULL || packet_len == NULL) {
        return false;
    }

    location_encode(&parsed->location, encoded_location, sizeof(encoded_location));
    type = type_to_u8(parsed->type);
    priority = priority_to_u8(parsed->priority);
    source_len = (uint8_t)strnlen(parsed->source, sizeof(parsed->source));
    destination_len = parsed->broadcast_destination || strcmp(parsed->destination, "ALL") == 0 ? V2_BROADCAST_ID : (uint8_t)strnlen(parsed->destination, sizeof(parsed->destination));
    relay_len = (uint8_t)strnlen(parsed->relay, sizeof(parsed->relay));
    payload_len = (uint8_t)strnlen(parsed->payload, sizeof(parsed->payload));
    if (parsed->payload_len > 0) {
        if (parsed->payload_len > sizeof(parsed->payload) || parsed->payload_len >= 255) {
            return false;
        }
        payload_len = (uint8_t)parsed->payload_len;
    }

    if (source_len == 0 || relay_len == 0 || payload_len >= sizeof(parsed->payload)) {
        return false;
    }
    if (parsed->next_hop[0] != '\0') {
        next_hop_len = (uint8_t)strnlen(parsed->next_hop, sizeof(parsed->next_hop));
        if (next_hop_len == 0) {
            return false;
        }
        flags |= V2_FLAG_NEXT_HOP;
    }
    if (parsed->location_raw[0] != '\0' || parsed->location.sitio[0] != '\0' || parsed->location.barangay[0] != '\0' || parsed->location.municipality[0] != '\0') {
        size_t encoded_len = strnlen(encoded_location, sizeof(encoded_location));
        if (encoded_len > 0 && encoded_len < 255) {
            flags |= V2_FLAG_LOCATION;
            location_len = (uint8_t)encoded_len;
        }
    }

    required = 2 + 1 + 1 + 1 + 1 + source_len + 1 + (destination_len == V2_BROADCAST_ID ? 0 : destination_len) + 4 + 1 + relay_len + 1 + (flags & V2_FLAG_NEXT_HOP ? 1 + next_hop_len : 0) + (flags & V2_FLAG_LOCATION ? 1 + location_len : 0) + 1 + payload_len;
    if (required > BEMS_MAX_PLAINTEXT || packet_size < required) {
        return false;
    }

    if (!write_u8(packet, packet_size, &offset, V2_MAGIC) ||
        !write_u8(packet, packet_size, &offset, BEMS_PACKET_FORMAT_V2) ||
        !write_u8(packet, packet_size, &offset, flags) ||
        !write_u8(packet, packet_size, &offset, type) ||
        !write_u8(packet, packet_size, &offset, priority) ||
        !write_u8(packet, packet_size, &offset, source_len) ||
        !write_bytes(packet, packet_size, &offset, (const uint8_t *)parsed->source, source_len) ||
        !write_u8(packet, packet_size, &offset, destination_len) ||
        (destination_len != V2_BROADCAST_ID && !write_bytes(packet, packet_size, &offset, (const uint8_t *)parsed->destination, destination_len)) ||
        !write_u32(packet, packet_size, &offset, parsed->id) ||
        !write_u8(packet, packet_size, &offset, relay_len) ||
        !write_bytes(packet, packet_size, &offset, (const uint8_t *)parsed->relay, relay_len) ||
        !write_u8(packet, packet_size, &offset, parsed->hops < 0 ? 0 : (uint8_t)parsed->hops)) {
        return false;
    }

    if ((flags & V2_FLAG_NEXT_HOP) != 0u) {
        if (!write_u8(packet, packet_size, &offset, next_hop_len) ||
            !write_bytes(packet, packet_size, &offset, (const uint8_t *)parsed->next_hop, next_hop_len)) {
            return false;
        }
    }

    if ((flags & V2_FLAG_LOCATION) != 0u) {
        if (!write_u8(packet, packet_size, &offset, location_len) ||
            !write_bytes(packet, packet_size, &offset, (const uint8_t *)encoded_location, location_len)) {
            return false;
        }
    }

    if (!write_u8(packet, packet_size, &offset, payload_len) ||
        !write_bytes(packet, packet_size, &offset, (const uint8_t *)parsed->payload, payload_len)) {
        return false;
    }

    *packet_len = offset;
    return offset <= BEMS_MAX_PLAINTEXT;
}

bool packet_seen(const char *source, uint32_t id)
{
    bool seen = false;
    uint32_t now_ms = dedup_now_ms();
    int bucket_index;
    int16_t index;

    ensure_mutex();
    dedup_init_storage();
    mesh_lock();
    expire_current_and_prior_buckets(now_ms);
    bucket_index = seen_bucket_index(source, id);
    index = hash_buckets[bucket_index];
    while (index != -1) {
        if (seen_packets[index].active && seen_entry_matches(&seen_packets[index], source, id)) {
            seen = true;
            break;
        }
        index = seen_packets[index].next_hash;
    }
    mesh_unlock();
    return seen;
}

void remember_packet(const char *source, uint32_t id)
{
    uint32_t now_ms = dedup_now_ms();
    TickType_t now_ticks = xTaskGetTickCount();
    int bucket_index;
    int16_t index;
    int16_t slot_index;

    ensure_mutex();
    dedup_init_storage();
    mesh_lock();
    expire_current_and_prior_buckets(now_ms);
    bucket_index = seen_bucket_index(source, id);
    index = hash_buckets[bucket_index];
    while (index != -1) {
        if (seen_packets[index].active && seen_entry_matches(&seen_packets[index], source, id)) {
            mesh_unlock();
            return;
        }
        index = seen_packets[index].next_hash;
    }

    slot_index = alloc_entry();
    if (slot_index == -1) {
        mesh_unlock();
        return;
    }

    insert_entry(slot_index, source, id, now_ticks);
    mesh_unlock();
}

void deduplication_debug_reset_for_test(void)
{
    ensure_mutex();
    dedup_init_storage();
    mesh_lock();
    memset(seen_packets, 0, sizeof(seen_packets));
    for (size_t i = 0; i < MAX_SEEN_BUCKETS; i++) {
        hash_buckets[i] = -1;
    }
    for (size_t i = 0; i < DEDUP_EXPIRY_BUCKET_COUNT; i++) {
        expiry_wheel[i] = -1;
    }
    free_list_head = 0;
    for (int16_t i = 0; i < MAX_SEEN_PACKETS - 1; i++) {
        seen_packets[i].next_hash = i + 1;
    }
    seen_packets[MAX_SEEN_PACKETS - 1].next_hash = -1;
    last_expiry_bucket = 0;
    mesh_unlock();
}

void deduplication_debug_set_seen_tick_for_test(const char *source, uint32_t id, TickType_t seen_tick)
{
    int bucket_index;
    int16_t index;

    ensure_mutex();
    dedup_init_storage();
    mesh_lock();
    bucket_index = seen_bucket_index(source, id);
    index = hash_buckets[bucket_index];
    while (index != -1) {
        if (seen_packets[index].active && seen_entry_matches(&seen_packets[index], source, id)) {
            seen_packets[index].seen_tick = seen_tick;
            break;
        }
        index = seen_packets[index].next_hash;
    }
    mesh_unlock();
}
