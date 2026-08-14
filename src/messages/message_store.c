#include "messages/message_store.h"

#include <stdbool.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include "storage/storage.h"
#include "utils/string_utils.h"
static SemaphoreHandle_t message_mutex;
static emergency_message_t messages[MAX_MESSAGES];
static emergency_message_t message_snapshot_buffer[MAX_MESSAGES];
static size_t message_count;

static void lock(void)
{
    if (message_mutex == NULL) {
        message_mutex = xSemaphoreCreateMutex();
    }
    if (message_mutex != NULL) {
        xSemaphoreTake(message_mutex, portMAX_DELAY);
    }
}

static void unlock(void)
{
    if (message_mutex != NULL) {
        xSemaphoreGive(message_mutex);
    }
}

static emergency_message_t *next_message_slot_locked(void)
{
    emergency_message_t *message = NULL;

    if (message_count < MAX_MESSAGES) {
        message = &messages[message_count++];
    } else {
        size_t oldest_completed_index = MAX_MESSAGES;
        TickType_t oldest_completed_tick = 0;

        for (size_t i = 0; i < MAX_MESSAGES; i++) {
            const emergency_message_t *candidate = &messages[i];
            bool completed = strcmp(candidate->direction, "RX") == 0 ||
                             strcmp(candidate->status, "ACKED") == 0 ||
                             strcmp(candidate->status, "FAILED") == 0;

            if (!completed) {
                continue;
            }

            if (oldest_completed_index == MAX_MESSAGES || candidate->stored_epoch <= oldest_completed_tick) {
                oldest_completed_index = i;
                oldest_completed_tick = candidate->stored_epoch;
            }
        }

        if (oldest_completed_index == MAX_MESSAGES) {
            return NULL;
        }

        message = &messages[oldest_completed_index];
    }

    memset(message, 0, sizeof(*message));
    return message;
}

static bool message_matches(const emergency_message_t *message, uint32_t id, const char *source)
{
    return message != NULL && message->id == id && strcmp(message->source, source) == 0;
}

static size_t find_message_index(uint32_t id, const char *source)
{
    for (size_t i = 0; i < message_count; i++) {
        if (message_matches(&messages[i], id, source)) {
            return i;
        }
    }
    return SIZE_MAX;
}

static void compact_remove_index(size_t index)
{
    if (index >= message_count) {
        return;
    }

    if (index + 1 < message_count) {
        memmove(&messages[index], &messages[index + 1], (message_count - index - 1) * sizeof(messages[0]));
    }
    memset(&messages[message_count - 1], 0, sizeof(messages[0]));
    message_count--;
}

void message_store_load_messages_from_nvs(const char *node_id)
{
    lock();
    message_count = 0;
    storage_message_load_messages(node_id, messages, &message_count, MAX_MESSAGES);
    unlock();
}

bool message_store_add(const emergency_message_t *message, int *nvs_slot)
{
    emergency_message_t *slot;
    int slot_index;

    if (message == NULL) {
        return false;
    }

    lock();
    slot = next_message_slot_locked();
    if (slot == NULL) {
        unlock();
        return false;
    }

    memcpy(slot, message, sizeof(*slot));
    slot_index = (int)(slot - messages);
    if (nvs_slot != NULL) {
        *nvs_slot = slot_index;
    }
    unlock();
    return true;
}

bool message_store_get(size_t index, emergency_message_t *out)
{
    bool found = false;

    if (out == NULL) {
        return false;
    }

    lock();
    if (index < message_count) {
        *out = messages[index];
        found = true;
    }
    unlock();
    return found;
}

bool message_store_find(uint32_t id, const char *source, emergency_message_t *out)
{
    bool found = false;
    size_t index;

    if (source == NULL || source[0] == '\0' || out == NULL) {
        return false;
    }

    lock();
    index = find_message_index(id, source);
    if (index != SIZE_MAX) {
        *out = messages[index];
        found = true;
    }
    unlock();
    return found;
}

bool message_store_remove(uint32_t id, const char *source)
{
    size_t index;

    if (source == NULL || source[0] == '\0') {
        return false;
    }

    lock();
    index = find_message_index(id, source);
    if (index == SIZE_MAX) {
        unlock();
        return false;
    }
    compact_remove_index(index);
    unlock();
    return true;
}

emergency_message_t *message_store_begin_write(int *nvs_slot)
{
    emergency_message_t *message;

    lock();
    message = next_message_slot_locked();
    if (nvs_slot != NULL) {
        *nvs_slot = message == NULL ? -1 : (int)(message - messages);
    }
    return message;
}

emergency_message_t *message_store_begin_update(uint32_t id, const char *source)
{
    emergency_message_t *message = NULL;

    lock();
    for (size_t i = 0; i < message_count; i++) {
        if (messages[i].id == id && strcmp(messages[i].source, source) == 0) {
            message = &messages[i];
            break;
        }
    }
    if (message == NULL) {
        unlock();
    }
    return message;
}

void message_store_end_update(void)
{
    unlock();
}

void message_store_update_status(uint32_t id, const char *source, const char *status)
{
    lock();
    for (size_t i = 0; i < message_count; i++) {
        emergency_message_t *message = &messages[i];
        if (message->id == id && strcmp(message->source, source) == 0) {
            copy_field(message->status, sizeof(message->status), status);
            break;
        }
    }
    unlock();
}

size_t message_store_copy_all(emergency_message_t *snapshot, size_t max_messages)
{
    size_t snapshot_count = 0;

    lock();
    for (size_t i = 0; i < message_count && snapshot_count < max_messages; i++) {
        snapshot[snapshot_count++] = messages[i];
    }
    unlock();
    return snapshot_count;
}

const emergency_message_t *message_store_snapshot(size_t *snapshot_count)
{
    size_t count = 0;

    lock();
    for (size_t i = 0; i < message_count && count < MAX_MESSAGES; i++) {
        message_snapshot_buffer[count++] = messages[message_count - 1 - i];
    }
    unlock();

    *snapshot_count = count;
    return message_snapshot_buffer;
}

size_t message_store_count(void)
{
    size_t count;

    lock();
    count = message_count;
    unlock();
    return count;
}
