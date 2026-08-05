#include "messages/message_store.h"

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "nvs.h"

#include "utils/string_utils.h"

#define CONFIG_NAMESPACE "bems_config"

static const char *TAG = "message_store";
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

void message_store_save_message_to_nvs(const emergency_message_t *message, int slot)
{
    nvs_handle_t handle;
    char key[16];
    esp_err_t result;

    if (slot < 0 || slot >= MAX_MESSAGES) {
        return;
    }

    result = nvs_open(CONFIG_NAMESPACE, NVS_READWRITE, &handle);
    if (result != ESP_OK) {
        ESP_LOGW(TAG, "Failed to open NVS for message save: %s", esp_err_to_name(result));
        return;
    }

    snprintf(key, sizeof(key), "msg_%d", slot);
    result = nvs_set_blob(handle, key, (const void *)message, sizeof(*message));
    if (result == ESP_OK) {
        result = nvs_commit(handle);
    }

    if (result != ESP_OK) {
        ESP_LOGW(TAG, "Failed to save message %d to NVS: %s", slot, esp_err_to_name(result));
    }

    nvs_close(handle);
}

void message_store_load_messages_from_nvs(const char *node_id)
{
    nvs_handle_t handle;
    char key[16];
    esp_err_t result;
    size_t blob_size;
    emergency_message_t loaded_message;

    result = nvs_open(CONFIG_NAMESPACE, NVS_READONLY, &handle);
    if (result != ESP_OK) {
        ESP_LOGI(TAG, "No saved messages in NVS");
        return;
    }

    lock();
    message_count = 0;
    for (int i = 0; i < MAX_MESSAGES; i++) {
        snprintf(key, sizeof(key), "msg_%d", i);
        blob_size = sizeof(loaded_message);
        result = nvs_get_blob(handle, key, &loaded_message, &blob_size);

        if (result == ESP_OK && blob_size == sizeof(loaded_message)) {
            if (strcmp(loaded_message.direction, "TX") == 0 || strcmp(loaded_message.direction, "RX") == 0) {
                // Only restore messages where we are source or destination (skip pure relay traffic)
                bool is_relevant = (strcmp(loaded_message.source, node_id) == 0) ||
                                   (strcmp(loaded_message.destination, node_id) == 0);
                if (is_relevant) {
                    memcpy(&messages[message_count], &loaded_message, sizeof(loaded_message));
                    message_count++;
                    if (message_count >= MAX_MESSAGES) {
                        break;
                    }
                }
            }
        }
    }
    unlock();

    ESP_LOGI(TAG, "Loaded %zu messages from NVS", message_count);
    nvs_close(handle);
}

emergency_message_t *message_store_begin_write(int *nvs_slot)
{
    emergency_message_t *message;

    lock();
    message = next_message_slot_locked();
    if (nvs_slot != NULL) {
        *nvs_slot = (int)(message_count - 1);
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
