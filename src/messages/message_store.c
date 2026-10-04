#include "messages/message_store.h"

#include <string.h>
#include <stdio.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "nvs.h"

#include "utils/string_utils.h"

static const char *MESSAGE_NAMESPACE = "bems_config";
static const uint32_t MESSAGE_STORE_MAGIC_VALUE = MESSAGE_STORE_MAGIC;
static StaticSemaphore_t message_mutex_buffer;
static SemaphoreHandle_t message_mutex;
static persisted_message_t persisted_slots[MAX_MESSAGES];
static bool reserved_slots[MAX_MESSAGES];
static size_t active_count;

static message_slot_state_t state_for_message(const emergency_message_t *message)
{
    if (message == NULL) return MESSAGE_SLOT_EMPTY;
    if (strcmp(message->direction, "RX") == 0) return MESSAGE_SLOT_RECEIVED;
    if (strcmp(message->status, "ACKED") == 0) return MESSAGE_SLOT_ACKED;
    if (strcmp(message->status, "FAILED") == 0 || strncmp(message->status, "FAILED_", 7) == 0) return MESSAGE_SLOT_FAILED;
    if (strcmp(message->status, "SENT") == 0) return MESSAGE_SLOT_SENT;
    return MESSAGE_SLOT_QUEUED;
}

static void lock(void)
{
    configASSERT(message_mutex != NULL);
    xSemaphoreTake(message_mutex, portMAX_DELAY);
}

static void unlock(void)
{
    configASSERT(message_mutex != NULL);
    xSemaphoreGive(message_mutex);
}

static uint32_t crc32_bytes(const uint8_t *data, size_t length)
{
    uint32_t crc = 0xFFFFFFFFu;

    for (size_t i = 0; i < length; i++) {
        crc ^= data[i];
        for (int bit = 0; bit < 8; bit++) {
            uint32_t mask = (uint32_t)-(int)(crc & 1u);
            crc = (crc >> 1) ^ (0xEDB88320u & mask);
        }
    }
    return ~crc;
}

static bool persisted_record_valid(const persisted_message_t *record)
{
    if (record == NULL) {
        return false;
    }
    if (record->magic != MESSAGE_STORE_MAGIC_VALUE || record->version != MESSAGE_STORE_VERSION) {
        return false;
    }
    if (record->state > MESSAGE_SLOT_RECEIVED) {
        return false;
    }
    if (record->length != sizeof(record->message)) {
        return false;
    }
    return record->crc32 == crc32_bytes((const uint8_t *)&record->message, record->length);
}

static const char *slot_key(int slot, char *buffer, size_t buffer_size)
{
    if (buffer == NULL || buffer_size == 0) {
        return NULL;
    }
    if (slot < 0 || slot >= MAX_MESSAGES) {
        return NULL;
    }
    snprintf(buffer, buffer_size, "msg_%d", slot);
    return buffer;
}

static void clear_slot(size_t slot)
{
    if (slot < MAX_MESSAGES) {
        memset(&persisted_slots[slot], 0, sizeof(persisted_slots[slot]));
    }
}

static void load_persisted_slot(nvs_handle_t handle, int slot)
{
    persisted_message_t record = {0};
    size_t blob_size = sizeof(record);
    char key[16];
    esp_err_t result;

    if (slot_key(slot, key, sizeof(key)) == NULL) {
        return;
    }

    result = nvs_get_blob(handle, key, &record, &blob_size);
    if (result != ESP_OK || blob_size != sizeof(record) || !persisted_record_valid(&record)) {
        clear_slot((size_t)slot);
        return;
    }

    persisted_slots[slot] = record;
}

static void recalculate_active_count(void)
{
    size_t count = 0;

    for (size_t i = 0; i < MAX_MESSAGES; i++) {
        if (persisted_slots[i].state != MESSAGE_SLOT_EMPTY) {
            count++;
        }
    }
    active_count = count;
}

static esp_err_t delete_record_nvs(int slot)
{
    nvs_handle_t handle;
    char key[16];
    esp_err_t result;

    if (slot < 0 || slot >= MAX_MESSAGES) {
        return ESP_ERR_INVALID_ARG;
    }

    result = nvs_open(MESSAGE_NAMESPACE, NVS_READWRITE, &handle);
    if (result != ESP_OK) {
        return result;
    }

    if (slot_key(slot, key, sizeof(key)) == NULL) {
        nvs_close(handle);
        return ESP_ERR_INVALID_ARG;
    }

    result = nvs_erase_key(handle, key);
    if (result == ESP_OK || result == ESP_ERR_NVS_NOT_FOUND) {
        result = nvs_commit(handle);
    }
    nvs_close(handle);
    return result;
}

static esp_err_t delete_record_locked(int slot)
{
    esp_err_t result = delete_record_nvs(slot);

    if (result == ESP_OK || result == ESP_ERR_NVS_NOT_FOUND) {
        clear_slot((size_t)slot);
        recalculate_active_count();
        return ESP_OK;
    }
    return result;
}

static bool message_matches(const emergency_message_t *message, uint32_t id, const char *source)
{
    return message != NULL && source != NULL && source[0] != '\0' &&
           message->id == id && strcmp(message->source, source) == 0;
}

static int find_message_slot(uint32_t id, const char *source)
{
    for (int i = 0; i < MAX_MESSAGES; i++) {
        if (persisted_slots[i].state != MESSAGE_SLOT_EMPTY &&
            message_matches(&persisted_slots[i].message, id, source)) {
            return i;
        }
    }
    return -1;
}

static void copy_persisted_to_message(const persisted_message_t *record, emergency_message_t *message)
{
    if (record != NULL && message != NULL) {
        *message = record->message;
    }
}

void message_store_load_messages_from_nvs(const char *node_id)
{
    nvs_handle_t handle;
    esp_err_t result;

    lock();
    memset(persisted_slots, 0, sizeof(persisted_slots));
    memset(reserved_slots, 0, sizeof(reserved_slots));
    active_count = 0;

    result = nvs_open(MESSAGE_NAMESPACE, NVS_READONLY, &handle);
    if (result != ESP_OK) {
        unlock();
        return;
    }

    for (int slot = 0; slot < MAX_MESSAGES; slot++) {
        load_persisted_slot(handle, slot);
    }
    nvs_close(handle);

    for (size_t slot = 0; slot < MAX_MESSAGES; slot++) {
        if (persisted_slots[slot].state == MESSAGE_SLOT_EMPTY) {
            continue;
        }
        if (node_id != NULL &&
            node_id[0] != '\0' &&
            strcmp(persisted_slots[slot].message.source, node_id) != 0 &&
            strcmp(persisted_slots[slot].message.destination, node_id) != 0) {
            clear_slot(slot);
            continue;
        }
    }

    recalculate_active_count();
    unlock();
}

esp_err_t message_store_init(void)
{
    if (message_mutex == NULL) {
        message_mutex = xSemaphoreCreateMutexStatic(&message_mutex_buffer);
        if (message_mutex == NULL) {
            return ESP_FAIL;
        }
    }
    return ESP_OK;
}

bool message_store_allocate(int *slot)
{
    if (slot == NULL) {
        return false;
    }

    lock();
    for (int i = 0; i < MAX_MESSAGES; i++) {
        if (persisted_slots[i].state == MESSAGE_SLOT_EMPTY && !reserved_slots[i]) {
            *slot = i;
            reserved_slots[i] = true;
            unlock();
            return true;
        }
    }
    int oldest_slot = -1;
    uint32_t oldest_epoch = UINT32_MAX;
    for (int i = 0; i < MAX_MESSAGES; i++) {
        message_slot_state_t state = state_for_message(&persisted_slots[i].message);
        if (reserved_slots[i] || (state != MESSAGE_SLOT_ACKED && state != MESSAGE_SLOT_FAILED && state != MESSAGE_SLOT_RECEIVED)) {
            continue;
        }
        uint32_t epoch = persisted_slots[i].message.stored_epoch;
        if (oldest_slot < 0 || (epoch != 0 && (oldest_epoch == 0 || epoch < oldest_epoch))) {
            oldest_slot = i;
            oldest_epoch = epoch;
        }
    }
    if (oldest_slot >= 0 && delete_record_locked(oldest_slot) == ESP_OK) {
        reserved_slots[oldest_slot] = true;
        *slot = oldest_slot;
        unlock();
        return true;
    }
    unlock();
    return false;
}

static esp_err_t write_record_locked(int slot, const emergency_message_t *message, message_slot_state_t state)
{
    persisted_message_t record = {0};
    nvs_handle_t handle;
    char key[16];
    esp_err_t result;

    if (message == NULL || slot < 0 || slot >= MAX_MESSAGES) {
        return ESP_ERR_INVALID_ARG;
    }

    record.magic = MESSAGE_STORE_MAGIC_VALUE;
    record.version = MESSAGE_STORE_VERSION;
    record.length = (uint16_t)sizeof(record.message);
    record.state = (uint16_t)state;
    record.message = *message;
    record.crc32 = crc32_bytes((const uint8_t *)&record.message, record.length);

    result = nvs_open(MESSAGE_NAMESPACE, NVS_READWRITE, &handle);
    if (result != ESP_OK) {
        return result;
    }
    if (slot_key(slot, key, sizeof(key)) == NULL) {
        nvs_close(handle);
        return ESP_ERR_INVALID_ARG;
    }

    result = nvs_set_blob(handle, key, &record, sizeof(record));
    if (result == ESP_OK) {
        result = nvs_commit(handle);
    }
    if (result == ESP_OK) {
        persisted_slots[slot] = record;
        recalculate_active_count();
    }
    nvs_close(handle);
    return result;
}

esp_err_t message_store_write(int slot, const emergency_message_t *message)
{
    esp_err_t result;
    if (message == NULL || slot < 0 || slot >= MAX_MESSAGES) return ESP_ERR_INVALID_ARG;
    lock();
    if (!reserved_slots[slot]) {
        unlock();
        return ESP_ERR_INVALID_STATE;
    }
    result = write_record_locked(slot, message, state_for_message(message));
    reserved_slots[slot] = false;
    unlock();
    return result;
}

esp_err_t message_store_update(int slot, const emergency_message_t *message)
{
    esp_err_t result;

    if (message == NULL || slot < 0 || slot >= MAX_MESSAGES) {
        return ESP_ERR_INVALID_ARG;
    }

    lock();
    if (reserved_slots[slot] && persisted_slots[slot].state == MESSAGE_SLOT_EMPTY) {
        result = write_record_locked(slot, message, state_for_message(message));
        reserved_slots[slot] = false;
    } else if (persisted_slots[slot].state != MESSAGE_SLOT_EMPTY) {
        result = write_record_locked(slot, message, state_for_message(message));
    } else {
        result = ESP_ERR_INVALID_STATE;
    }
    unlock();
    return result;
}

esp_err_t message_store_delete(int slot)
{
    esp_err_t result;

    if (slot < 0 || slot >= MAX_MESSAGES) {
        return ESP_ERR_INVALID_ARG;
    }

    lock();
    if (reserved_slots[slot] && persisted_slots[slot].state == MESSAGE_SLOT_EMPTY) {
        reserved_slots[slot] = false;
        result = ESP_OK;
    } else {
        result = delete_record_locked(slot);
    }
    unlock();
    return result;
}

bool message_store_read(int slot, emergency_message_t *message)
{
    bool found = false;

    if (message == NULL || slot < 0 || slot >= MAX_MESSAGES) {
        return false;
    }

    lock();
    if (persisted_slots[slot].state != MESSAGE_SLOT_EMPTY &&
        persisted_record_valid(&persisted_slots[slot])) {
        copy_persisted_to_message(&persisted_slots[slot], message);
        found = true;
    }
    unlock();
    return found;
}

bool message_store_get(size_t index, emergency_message_t *out)
{
    size_t count = 0;

    if (out == NULL) {
        return false;
    }

    lock();
    for (size_t slot = 0; slot < MAX_MESSAGES; slot++) {
        if (persisted_slots[slot].state == MESSAGE_SLOT_EMPTY) {
            continue;
        }
        if (count++ == index) {
            copy_persisted_to_message(&persisted_slots[slot], out);
            unlock();
            return true;
        }
    }
    unlock();
    return false;
}

bool message_store_find(uint32_t id, const char *source, emergency_message_t *out)
{
    int slot;

    if (out == NULL || source == NULL || source[0] == '\0') {
        return false;
    }

    lock();
    slot = find_message_slot(id, source);
    if (slot >= 0) {
        copy_persisted_to_message(&persisted_slots[slot], out);
        unlock();
        return true;
    }
    unlock();
    return false;
}

bool message_store_remove(uint32_t id, const char *source)
{
    int slot;
    esp_err_t result;

    if (source == NULL || source[0] == '\0') {
        return false;
    }

    lock();
    slot = find_message_slot(id, source);
    result = slot >= 0 ? delete_record_locked(slot) : ESP_ERR_NVS_NOT_FOUND;
    unlock();
    return result == ESP_OK;
}

void message_store_update_status(uint32_t id, const char *source, const char *status)
{
    int slot;
    emergency_message_t message;

    if (source == NULL || source[0] == '\0' || status == NULL) {
        return;
    }

    lock();
    slot = find_message_slot(id, source);
    if (slot < 0) {
        unlock();
        return;
    }
    message = persisted_slots[slot].message;
    copy_field(message.status, sizeof(message.status), status);
    // Keep the lock held across the whole read-modify-write so a concurrent
    // caller cannot modify the same slot between our read and our write.
    (void)write_record_locked(slot, &message, state_for_message(&message));
    unlock();
}

size_t message_store_copy_all(emergency_message_t *snapshot, size_t max_messages)
{
    size_t snapshot_count = 0;

    if (snapshot == NULL || max_messages == 0) {
        return 0;
    }

    lock();
    for (size_t slot = 0; slot < MAX_MESSAGES && snapshot_count < max_messages; slot++) {
        if (persisted_slots[slot].state == MESSAGE_SLOT_EMPTY) {
            continue;
        }
        snapshot[snapshot_count++] = persisted_slots[slot].message;
    }
    unlock();
    return snapshot_count;
}

size_t message_store_count(void)
{
    size_t count;

    lock();
    count = active_count;
    unlock();
    return count;
}

size_t message_store_copy_pending_tx(emergency_message_t *messages, int *slots, size_t capacity)
{
    size_t count = 0;
    if (messages == NULL || slots == NULL || capacity == 0) return 0;
    lock();
    for (int slot = 0; slot < MAX_MESSAGES && count < capacity; slot++) {
        message_slot_state_t state = state_for_message(&persisted_slots[slot].message);
        if ((state == MESSAGE_SLOT_QUEUED || state == MESSAGE_SLOT_SENT) &&
            strcmp(persisted_slots[slot].message.direction, "TX") == 0 &&
            strcmp(persisted_slots[slot].message.status, "ACKED") != 0 &&
            strncmp(persisted_slots[slot].message.status, "FAILED", 6) != 0) {
            messages[count] = persisted_slots[slot].message;
            slots[count] = slot;
            count++;
        }
    }
    unlock();
    return count;
}
