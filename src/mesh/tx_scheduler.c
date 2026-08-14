#include "mesh/tx_scheduler.h"

#include <string.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "mesh_control.h"
#include "mesh_protocol.h"
#include "messages/message_store.h"
#include "radio/lora_radio.h"
#include "route_table.h"
#include "storage/storage.h"
#include "utils/string_utils.h"

#define TX_RETRY_MAX_ATTEMPTS 3
#define TX_ACK_TIMEOUT_MS 5000

typedef enum {
    TX_STATE_EMPTY = 0,
    TX_STATE_QUEUED,
    TX_STATE_SENDING,
    TX_STATE_WAITING_ACK,
    TX_STATE_RETRY_PENDING,
    TX_STATE_DELIVERED,
    TX_STATE_FAILED,
} tx_state_t;

typedef struct {
    emergency_message_t message;
    int nvs_slot;
    tx_state_t state;
    uint8_t attempts;
    TickType_t next_action_tick;
    bool active;
} tx_entry_t;

static const char *TAG = "tx_scheduler";
static SemaphoreHandle_t tx_mutex;
static tx_entry_t tx_entries[MAX_MESSAGES];
static TaskHandle_t tx_task_handle;

static void lock(void)
{
    if (tx_mutex == NULL) {
        tx_mutex = xSemaphoreCreateMutex();
    }
    if (tx_mutex != NULL) {
        xSemaphoreTake(tx_mutex, portMAX_DELAY);
    }
}

static void unlock(void)
{
    if (tx_mutex != NULL) {
        xSemaphoreGive(tx_mutex);
    }
}

static tx_entry_t *find_entry(uint32_t id, const char *ack_sender)
{
    for (size_t i = 0; i < MAX_MESSAGES; i++) {
        tx_entry_t *entry = &tx_entries[i];
        /*
         * ACKs are received from the packet destination, so match against the
         * original queued message destination rather than the sender field.
         */
        if (entry->active &&
            entry->message.id == id &&
            strcmp(entry->message.destination, ack_sender) == 0) {
            return entry;
        }
    }
    return NULL;
}

static tx_entry_t *next_slot(void)
{
    for (size_t i = 0; i < MAX_MESSAGES; i++) {
        if (!tx_entries[i].active) {
            return &tx_entries[i];
        }
    }
    return NULL;
}

static void set_status(tx_entry_t *entry, const char *status)
{
    copy_field(entry->message.status, sizeof(entry->message.status), status);
    if (entry->nvs_slot >= 0) {
        storage_message_save(&entry->message, entry->nvs_slot);
    }
    message_store_update_status(entry->message.id, entry->message.source, status);
}

static void send_current_packet(tx_entry_t *entry)
{
    mesh_packet_t packet = {0};
    uint8_t route_packet[PACKET_LEN];
    size_t route_packet_len = 0;
    route_entry_t route = {0};

    if (route_table_get_best(entry->message.destination, &route)) {
        ESP_LOGI(TAG, "TX %s -> %s via %s (hop=%d rssi=%d cost=%lu)",
                 entry->message.source,
                 entry->message.destination,
                 route.next_hop,
                 route.hop_count,
                 route.best_rssi,
                 (unsigned long)route.cost);
    }
    packet.valid = true;
    packet.id = entry->message.id;
    packet.hops = entry->message.hops;
    copy_field(packet.source, sizeof(packet.source), entry->message.source);
    copy_field(packet.destination, sizeof(packet.destination), entry->message.destination);
    copy_field(packet.type, sizeof(packet.type), entry->message.type);
    copy_field(packet.priority, sizeof(packet.priority), entry->message.priority);
    copy_field(packet.relay, sizeof(packet.relay), entry->message.source);
    copy_field(packet.payload, sizeof(packet.payload), entry->message.payload);
    if (!build_forward_packet_v2(&packet, route_packet, sizeof(route_packet), &route_packet_len)) {
        entry->state = TX_STATE_RETRY_PENDING;
        entry->next_action_tick = xTaskGetTickCount() + pdMS_TO_TICKS(TX_ACK_TIMEOUT_MS);
        return;
    }
    if (lora_transmit_bytes(route_packet, route_packet_len)) {
        entry->attempts++;
        entry->state = TX_STATE_WAITING_ACK;
        entry->next_action_tick = xTaskGetTickCount() + pdMS_TO_TICKS(TX_ACK_TIMEOUT_MS);
        set_status(entry, "SENT");
    } else {
        entry->state = TX_STATE_RETRY_PENDING;
        entry->next_action_tick = xTaskGetTickCount() + pdMS_TO_TICKS(TX_ACK_TIMEOUT_MS);
    }
}

static void finalize_failure(tx_entry_t *entry)
{
    entry->state = TX_STATE_FAILED;
    set_status(entry, "FAILED");
    entry->active = false;
}

static void scheduler_task(void *parameter)
{
    (void)parameter;
    while (true) {
        TickType_t now = xTaskGetTickCount();

        lock();
        for (size_t i = 0; i < MAX_MESSAGES; i++) {
            tx_entry_t *entry = &tx_entries[i];

            if (!entry->active) {
                continue;
            }

            if (entry->state == TX_STATE_QUEUED || entry->state == TX_STATE_RETRY_PENDING) {
                if (now >= entry->next_action_tick) {
                    entry->state = TX_STATE_SENDING;
                    send_current_packet(entry);
                }
            } else if (entry->state == TX_STATE_WAITING_ACK && now >= entry->next_action_tick) {
                if (entry->attempts >= TX_RETRY_MAX_ATTEMPTS) {
                    finalize_failure(entry);
                } else {
                    entry->state = TX_STATE_RETRY_PENDING;
                    entry->next_action_tick = now + pdMS_TO_TICKS(5000 * entry->attempts);
                }
            }
        }
        unlock();

        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

void tx_scheduler_init(void)
{
    if (tx_mutex == NULL) {
        tx_mutex = xSemaphoreCreateMutex();
    }
    if (tx_task_handle == NULL) {
        xTaskCreate(scheduler_task, "tx_scheduler", 4096, NULL, 4, &tx_task_handle);
    }
}

bool tx_scheduler_enqueue(const emergency_message_t *message, int nvs_slot)
{
    tx_entry_t *entry;

    if (message == NULL) {
        return false;
    }

    tx_scheduler_init();
    lock();
    entry = next_slot();
    if (entry == NULL) {
        unlock();
        return false;
    }

    memset(entry, 0, sizeof(*entry));
    entry->message = *message;
    entry->nvs_slot = nvs_slot;
    entry->state = TX_STATE_QUEUED;
    entry->attempts = 0;
    entry->next_action_tick = xTaskGetTickCount();
    entry->active = true;
    set_status(entry, "QUEUED");
    unlock();
    return true;
}

bool tx_scheduler_acknowledge(uint32_t acknowledged_id, const char *ack_source)
{
    tx_entry_t *entry;

    if (ack_source == NULL || ack_source[0] == '\0') {
        return false;
    }

    lock();
    entry = find_entry(acknowledged_id, ack_source);
    if (entry == NULL) {
        unlock();
        return false;
    }
    entry->state = TX_STATE_DELIVERED;
    set_status(entry, "ACKED");
    entry->active = false;
    unlock();
    return true;
}

size_t tx_scheduler_queue_depth(void)
{
    size_t depth = 0;

    lock();
    for (size_t i = 0; i < MAX_MESSAGES; i++) {
        if (tx_entries[i].active) {
            depth++;
        }
    }
    unlock();
    return depth;
}

void tx_scheduler_debug_reset_for_test(void)
{
    lock();
    memset(tx_entries, 0, sizeof(tx_entries));
    unlock();
}
