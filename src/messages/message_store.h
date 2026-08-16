#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "bems_common.h"

#define MAX_MESSAGES 16
#define PAYLOAD_LEN 140
#define PACKET_LEN 320

#define MESSAGE_STORE_MAGIC 0x424D5347u
#define MESSAGE_STORE_VERSION 1u

typedef enum {
    MESSAGE_SLOT_EMPTY = 0,
    MESSAGE_SLOT_QUEUED = 1,
    MESSAGE_SLOT_SENT = 2,
    MESSAGE_SLOT_ACKED = 3,
    MESSAGE_SLOT_FAILED = 4,
} message_slot_state_t;

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

typedef struct {
    uint32_t magic;
    uint16_t version;
    uint16_t length;
    uint32_t crc32;
    uint16_t state;
    emergency_message_t message;
} persisted_message_t;

esp_err_t message_store_init(void);
void message_store_load_messages_from_nvs(const char *node_id);
bool message_store_allocate(int *slot);
esp_err_t message_store_write(int slot, const emergency_message_t *message);
esp_err_t message_store_update(int slot, const emergency_message_t *message);
esp_err_t message_store_delete(int slot);
bool message_store_read(int slot, emergency_message_t *message);

bool message_store_get(size_t index, emergency_message_t *out);
bool message_store_find(uint32_t id, const char *source, emergency_message_t *out);
bool message_store_remove(uint32_t id, const char *source);
emergency_message_t *message_store_begin_write(int *nvs_slot);
emergency_message_t *message_store_begin_update(uint32_t id, const char *source);
void message_store_end_update(void);
void message_store_update_status(uint32_t id, const char *source, const char *status);
size_t message_store_copy_all(emergency_message_t *snapshot, size_t max_messages);
const emergency_message_t *message_store_snapshot(size_t *snapshot_count);
size_t message_store_count(void);
