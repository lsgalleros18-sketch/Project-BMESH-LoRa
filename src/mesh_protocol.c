#include "mesh_protocol.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

enum {
    DEDUP_EXPIRY_BUCKET_MS = 1000,
    DEDUP_EXPIRY_BUCKET_COUNT = SEEN_PACKET_TTL_MS / DEDUP_EXPIRY_BUCKET_MS,
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

static void expire_current_and_prior_buckets(uint32_t now_ms)
{
    uint32_t current_bucket = now_ms / DEDUP_EXPIRY_BUCKET_MS;
    uint32_t bucket_span = current_bucket - last_expiry_bucket;

    if (bucket_span > DEDUP_EXPIRY_BUCKET_COUNT) {
        bucket_span = DEDUP_EXPIRY_BUCKET_COUNT;
    }

    for (uint32_t step = 0; step < bucket_span; step++) {
        uint32_t wheel_index = last_expiry_bucket % DEDUP_EXPIRY_BUCKET_COUNT;
        int16_t index = expiry_wheel[last_expiry_bucket % DEDUP_EXPIRY_BUCKET_COUNT];
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
              48,
              parsed->payload);
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
