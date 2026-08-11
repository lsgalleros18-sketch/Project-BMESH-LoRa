#include "mesh/mesh_retry.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "esp_err.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "messages/message_store.h"
#include "radio/lora_radio.h"
#include "utils/string_utils.h"

#define FIELD_LEN 32

typedef struct {
    uint32_t id;
    char source[FIELD_LEN];
    char destination[FIELD_LEN];
    char priority[FIELD_LEN];
    uint8_t attempts;
    TickType_t next_retry_tick;
    bool active;
} retry_entry_t;

static retry_entry_t retry_entries[MAX_MESSAGES];
static SemaphoreHandle_t retry_mutex;

static void retry_tracker_task(void *parameter)
{
    while (true) {
        TickType_t now = xTaskGetTickCount();
        xSemaphoreTake(retry_mutex, portMAX_DELAY);
        for (size_t i = 0; i < MAX_MESSAGES; i++) {
            retry_entry_t *entry = &retry_entries[i];
            if (!entry->active || now < entry->next_retry_tick) {
                continue;
            }

            emergency_message_t *message = message_store_begin_update(entry->id, entry->source);
            if (message == NULL) {
                continue;
            }
            if (strcmp(message->status, "ACKED") == 0) {
                entry->active = false;
            } else if (entry->attempts >= 3) {
                copy_field(message->status, sizeof(message->status), "FAILED");
                entry->active = false;
            } else {
                entry->attempts++;
                entry->next_retry_tick = now + pdMS_TO_TICKS(5000 * entry->attempts);
                lora_transmit(message->packet);
                copy_field(message->status, sizeof(message->status), "SENT");
            }
            message_store_end_update();
        }
        xSemaphoreGive(retry_mutex);
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

void mesh_retry_init(void)
{
    retry_mutex = xSemaphoreCreateMutex();
    ESP_ERROR_CHECK(retry_mutex == NULL ? ESP_ERR_NO_MEM : ESP_OK);
    xTaskCreate(retry_tracker_task, "retry_tracker_task", 4096, NULL, 3, NULL);
}

void mesh_retry_track(uint32_t id, const char *source, const char *destination, const char *priority)
{
    if (strcmp(priority, "HIGH") != 0 || strcmp(destination, "ALL") == 0) {
        return;
    }

    xSemaphoreTake(retry_mutex, portMAX_DELAY);
    for (size_t i = 0; i < MAX_MESSAGES; i++) {
        retry_entry_t *entry = &retry_entries[i];
        if (!entry->active) {
            entry->id = id;
            copy_field(entry->source, sizeof(entry->source), source);
            copy_field(entry->destination, sizeof(entry->destination), destination);
            copy_field(entry->priority, sizeof(entry->priority), priority);
            entry->attempts = 1;
            entry->next_retry_tick = xTaskGetTickCount() + pdMS_TO_TICKS(5000);
            entry->active = true;
            break;
        }
    }
    xSemaphoreGive(retry_mutex);
}
