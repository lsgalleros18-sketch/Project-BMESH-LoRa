#include "mesh_control.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "esp_random.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "nvs.h"

#include "bems_crypto.h"
#include "mesh_protocol.h"
#include "mesh/mesh_sequence.h"
#include "messages/message_store.h"
#include "node_config.h"
#include "radio/lora_radio.h"
#include "utils/string_utils.h"

static const char *TAG = "barangay_mesh";
static int64_t epoch_offset_sec;
static bool time_synced;
static uint8_t time_sync_distance;
static TickType_t last_time_sync_broadcast_tick;
static uint32_t sync_last_id_from_payload(const char *payload);
static uint32_t time_sync_epoch_from_payload(const char *payload);
static uint8_t time_sync_dist_from_payload(const char *payload);
bool mesh_control_is_control_packet_type(const char *type);
bool mesh_control_handle_time_sync_packet(const char *payload);
static bool is_private_destination_for_other_node(const char *destination, const char *requester);
void mesh_control_send_ack_packet(const mesh_packet_t *parsed);
void mesh_control_send_sync_responses(const mesh_packet_t *request);

void mesh_control_load_highest_seen_id(void)
{
    mesh_sequence_init();
}

uint32_t mesh_control_get_highest_seen_id(void)
{
    return mesh_sequence_peek();
}

void mesh_control_update_highest_seen_id(uint32_t id)
{
    mesh_sequence_update(id);
}

uint32_t mesh_control_current_epoch_seconds(void)
{
    return (uint32_t)(epoch_offset_sec + (int64_t)(xTaskGetTickCount() / configTICK_RATE_HZ));
}

void mesh_control_apply_time_sync(uint32_t epoch, uint8_t distance)
{
    uint32_t now_ticks = xTaskGetTickCount();
    uint32_t now_seconds = now_ticks / configTICK_RATE_HZ;

    epoch_offset_sec = (int64_t)epoch - (int64_t)now_seconds;
    time_synced = true;
    time_sync_distance = distance;
    last_time_sync_broadcast_tick = 0;
}

bool mesh_control_is_time_synced(void)
{
    return time_synced;
}

const bool *mesh_control_time_synced_ref(void)
{
    return &time_synced;
}

uint8_t mesh_control_get_time_sync_distance(void)
{
    return time_sync_distance;
}

void mesh_control_send_time_sync_packet(uint32_t epoch, uint8_t distance, uint8_t hops)
{
    char packet[PACKET_LEN];
    char encoded_location[SITIO_LEN + BARANGAY_LEN + MUNICIPALITY_LEN + 2];
    uint32_t message_id;

    message_id = mesh_sequence_next();
    mesh_sequence_save();
    location_encode(&node_config_get()->location, encoded_location, sizeof(encoded_location));

    snprintf(packet, sizeof(packet), "BEMS|%lu|%.*s|ALL|TIME_SYNC|NORMAL|HOPS=%u|RELAY=%.*s|LOC=%s|epoch=%lu~dist=%u",
             (unsigned long)message_id,
             31,
             node_config_get_node_id(),
             hops,
             31,
             node_config_get_node_id(),
             encoded_location,
             (unsigned long)epoch,
             distance);
    lora_transmit(packet);
}

void mesh_control_broadcast_time_sync_if_synced(void)
{
    if (!time_synced) {
        return;
    }

    if ((xTaskGetTickCount() - last_time_sync_broadcast_tick) >= pdMS_TO_TICKS(15 * 60 * 1000)) {
        mesh_control_send_time_sync_packet(mesh_control_current_epoch_seconds(), time_sync_distance, 2);
        last_time_sync_broadcast_tick = xTaskGetTickCount();
    }
}

void mesh_control_send_sync_request(uint32_t last_id)
{
    char sync_request[PACKET_LEN];
    char encoded_location[SITIO_LEN + BARANGAY_LEN + MUNICIPALITY_LEN + 2];
    uint32_t request_id;

    request_id = mesh_sequence_next();
    mesh_sequence_save();
    location_encode(&node_config_get()->location, encoded_location, sizeof(encoded_location));

    snprintf(sync_request, sizeof(sync_request), "BEMS|%lu|%.*s|ALL|SYNC_REQ|NORMAL|HOPS=1|RELAY=%.*s|LOC=%s|last_id=%lu",
             (unsigned long)request_id,
             31,
             node_config_get_node_id(),
             31,
             node_config_get_node_id(),
             encoded_location,
             (unsigned long)last_id);

    ESP_LOGI(TAG, "Broadcasting SYNC_REQ with last_id=%lu", (unsigned long)last_id);
    lora_transmit(sync_request);
}

void mesh_control_send_boot_sync_request(void)
{
    mesh_control_send_sync_request(mesh_sequence_peek());
}

void mesh_control_send_manual_sync_request(void)
{
    mesh_control_send_sync_request(0);
}

static uint32_t sync_last_id_from_payload(const char *payload)
{
    if (strncmp(payload, "last_id=", 8) == 0) {
        return (uint32_t)strtoul(payload + 8, NULL, 10);
    }
    return 0;
}

static uint32_t time_sync_epoch_from_payload(const char *payload)
{
    if (strncmp(payload, "epoch=", 6) == 0) {
        return (uint32_t)strtoul(payload + 6, NULL, 10);
    }
    return 0;
}

static uint8_t time_sync_dist_from_payload(const char *payload)
{
    const char *dist_ptr = strstr(payload, "~dist=");
    if (dist_ptr != NULL) {
        return (uint8_t)strtoul(dist_ptr + 6, NULL, 10);
    }
    return 0;
}

bool mesh_control_is_control_packet_type(const char *type)
{
    return strcmp(type, "ACK") == 0 || strcmp(type, "SYNC_REQ") == 0 || strcmp(type, "SYNC_RESP") == 0 || strcmp(type, "TIME_SYNC") == 0;
}

bool mesh_control_handle_time_sync_packet(const char *payload)
{
    uint32_t epoch = time_sync_epoch_from_payload(payload);
    uint8_t dist = time_sync_dist_from_payload(payload);

    if (epoch == 0) {
        return false;
    }

    if (!mesh_control_is_time_synced() || dist < mesh_control_get_time_sync_distance()) {
        mesh_control_apply_time_sync(epoch, dist);
    }

    return true;
}

static bool is_private_destination_for_other_node(const char *destination, const char *requester)
{
    return strcmp(destination, "ALL") != 0 && strcmp(destination, requester) != 0;
}

void mesh_control_send_ack_packet(const mesh_packet_t *parsed)
{
    char ack_packet[PACKET_LEN];
    char encoded_location[SITIO_LEN + BARANGAY_LEN + MUNICIPALITY_LEN + 2];
    uint32_t ack_id;

    ack_id = mesh_sequence_next();
    mesh_sequence_save();
    location_encode(&node_config_get()->location, encoded_location, sizeof(encoded_location));

    snprintf(ack_packet, sizeof(ack_packet), "BEMS|%lu|%.*s|%.*s|ACK|NORMAL|HOPS=0|RELAY=%.*s|LOC=%s|ACK for %lu",
             (unsigned long)ack_id,
             31,
             node_config_get_node_id(),
             31,
             parsed->source,
             31,
             node_config_get_node_id(),
             encoded_location,
             (unsigned long)parsed->id);

    ESP_LOGI(TAG, "Sending ACK to %s for packet %lu", parsed->source, (unsigned long)parsed->id);
    lora_transmit(ack_packet);
}

void mesh_control_send_sync_responses(const mesh_packet_t *request)
{
    emergency_message_t snapshot[MAX_MESSAGES];
    size_t snapshot_count = 0;
    size_t stored_message_count;
    uint32_t last_id = sync_last_id_from_payload(request->payload);
    char encoded_location[SITIO_LEN + BARANGAY_LEN + MUNICIPALITY_LEN + 2];

    location_encode(&node_config_get()->location, encoded_location, sizeof(encoded_location));

    stored_message_count = message_store_copy_all(snapshot, MAX_MESSAGES);
    for (size_t i = 0; i < stored_message_count; i++) {
        const emergency_message_t *message = &snapshot[i];

        if (strcmp(message->direction, "RX") != 0 && strcmp(message->direction, "TX") != 0) {
            continue;
        }
        if (message->id <= last_id) {
            continue;
        }
        if (strcmp(message->destination, "ALL") != 0 && strcmp(message->destination, request->source) != 0) {
            continue;
        }
        if (is_private_destination_for_other_node(message->destination, request->source)) {
            continue;
        }

        snapshot[snapshot_count++] = *message;
    }

    for (size_t i = 0; i < snapshot_count; i++) {
        char sync_response[PACKET_LEN];
        const char *original_packet = strstr(snapshot[i].packet, "BEMS|");
        int prefix_len;
        size_t original_len;
        uint32_t response_id;

        vTaskDelay(pdMS_TO_TICKS(200 + (esp_random() % 1001)));
        if (original_packet == NULL) {
            original_packet = snapshot[i].packet;
        }

        response_id = mesh_sequence_next();
        mesh_sequence_save();
        prefix_len = snprintf(sync_response, sizeof(sync_response), "BEMS|%lu|%.*s|%.*s|SYNC_RESP|NORMAL|HOPS=0|RELAY=%.*s|LOC=%s|",
                              (unsigned long)response_id,
                              31,
                              node_config_get_node_id(),
                              31,
                              request->source,
                              31,
                              node_config_get_node_id(),
                              encoded_location);
        original_len = strlen(original_packet);

        if (prefix_len < 0 || (size_t)prefix_len >= sizeof(sync_response) || (size_t)prefix_len + original_len > BEMS_MAX_PLAINTEXT) {
            ESP_LOGW(TAG, "Skipping oversized SYNC_RESP for packet %lu", (unsigned long)response_id);
            continue;
        }
        copy_field(sync_response + prefix_len, sizeof(sync_response) - (size_t)prefix_len, original_packet);

        ESP_LOGI(TAG, "SYNC_RESP to %s for packet %lu", request->source, (unsigned long)response_id);
        lora_transmit(sync_response);
    }
}
