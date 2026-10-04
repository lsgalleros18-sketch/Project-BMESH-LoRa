#include "mesh_control.h"

#include "mesh/forward_queue.h"
#include "mesh/forward_worker.h"
#include "route_table.h"

#include <stdbool.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "esp_random.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/queue.h"
#include "freertos/task.h"
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
static uint32_t sync_cursor_for_source(const uint8_t *payload, size_t payload_len, const char *source);
static uint32_t time_sync_epoch_from_payload(const char *payload);
static uint8_t time_sync_dist_from_payload(const char *payload);
bool mesh_control_is_control_packet_type(const char *type);
bool mesh_control_handle_time_sync_packet(const char *payload);
static bool is_private_destination_for_other_node(const char *destination, const char *requester);
bool mesh_control_build_ack(const mesh_packet_t *parsed, ack_info_t *ack_info);
bool mesh_control_parse_ack(const uint8_t *payload, size_t payload_len, ack_info_t *ack_info);
void mesh_control_send_ack_packet(const mesh_packet_t *parsed);
void mesh_control_send_sync_responses(const mesh_packet_t *request);

#define SYNC_RESP_RECORD_FIXED_LEN 10u
#define SYNC_RESP_RECORD_COUNT_LEN 1u

static bool sync_response_record_fits(const emergency_message_t *message)
{
    if (message == NULL) {
        return false;
    }
    return strnlen(message->source, sizeof(message->source)) < 255 &&
           strnlen(message->destination, sizeof(message->destination)) < 255 &&
           strnlen(message->type, sizeof(message->type)) < 255 &&
           strnlen(message->priority, sizeof(message->priority)) < 255 &&
           strnlen(message->payload, sizeof(message->payload)) < 255 &&
           message->hops >= 0 && message->hops <= 255;
}

static bool sync_response_write_u8(uint8_t *buffer, size_t buffer_size, size_t *offset, uint8_t value)
{
    if (*offset >= buffer_size) {
        return false;
    }
    buffer[(*offset)++] = value;
    return true;
}

static bool sync_response_write_u32(uint8_t *buffer, size_t buffer_size, size_t *offset, uint32_t value)
{
    if (*offset + 4 > buffer_size) {
        return false;
    }
    buffer[(*offset)++] = (uint8_t)(value & 0xFF);
    buffer[(*offset)++] = (uint8_t)((value >> 8) & 0xFF);
    buffer[(*offset)++] = (uint8_t)((value >> 16) & 0xFF);
    buffer[(*offset)++] = (uint8_t)((value >> 24) & 0xFF);
    return true;
}

static bool sync_response_write_bytes(uint8_t *buffer, size_t buffer_size, size_t *offset, const uint8_t *data, size_t len)
{
    if (*offset + len > buffer_size) {
        return false;
    }
    if (len > 0) {
        memcpy(&buffer[*offset], data, len);
        *offset += len;
    }
    return true;
}

static size_t sync_response_record_encoded_size(const emergency_message_t *message)
{
    return SYNC_RESP_RECORD_FIXED_LEN +
           strnlen(message->source, sizeof(message->source)) +
           strnlen(message->destination, sizeof(message->destination)) +
           strnlen(message->type, sizeof(message->type)) +
           strnlen(message->priority, sizeof(message->priority)) +
           strnlen(message->payload, sizeof(message->payload));
}

static bool sync_response_write_record(uint8_t *buffer, size_t buffer_size, size_t *offset, const emergency_message_t *message)
{
    uint8_t source_len = (uint8_t)strnlen(message->source, sizeof(message->source));
    uint8_t destination_len = (uint8_t)strnlen(message->destination, sizeof(message->destination));
    uint8_t type_len = (uint8_t)strnlen(message->type, sizeof(message->type));
    uint8_t priority_len = (uint8_t)strnlen(message->priority, sizeof(message->priority));
    uint8_t payload_len = (uint8_t)strnlen(message->payload, sizeof(message->payload));
    size_t required = SYNC_RESP_RECORD_FIXED_LEN + source_len + destination_len + type_len + priority_len + payload_len;

    if (message == NULL || offset == NULL || *offset + required > buffer_size || !sync_response_record_fits(message)) {
        return false;
    }

    if (!sync_response_write_u32(buffer, buffer_size, offset, message->id) ||
        !sync_response_write_u8(buffer, buffer_size, offset, source_len) ||
        !sync_response_write_bytes(buffer, buffer_size, offset, (const uint8_t *)message->source, source_len) ||
        !sync_response_write_u8(buffer, buffer_size, offset, destination_len) ||
        !sync_response_write_bytes(buffer, buffer_size, offset, (const uint8_t *)message->destination, destination_len) ||
        !sync_response_write_u8(buffer, buffer_size, offset, type_len) ||
        !sync_response_write_bytes(buffer, buffer_size, offset, (const uint8_t *)message->type, type_len) ||
        !sync_response_write_u8(buffer, buffer_size, offset, priority_len) ||
        !sync_response_write_bytes(buffer, buffer_size, offset, (const uint8_t *)message->priority, priority_len) ||
        !sync_response_write_u8(buffer, buffer_size, offset, (uint8_t)message->hops) ||
        !sync_response_write_u8(buffer, buffer_size, offset, payload_len) ||
        !sync_response_write_bytes(buffer, buffer_size, offset, (const uint8_t *)message->payload, payload_len)) {
        return false;
    }

    return true;
}

bool mesh_control_encode_sync_response_batch(const emergency_message_t *records, size_t record_count, uint8_t *body, size_t body_size, size_t *body_len)
{
    size_t offset = 0;

    if (records == NULL || body == NULL || body_len == NULL || record_count > 255 || body_size < 1) {
        return false;
    }

    body[offset++] = (uint8_t)record_count;
    for (size_t i = 0; i < record_count; i++) {
        if (!sync_response_write_record(body, body_size, &offset, &records[i])) {
            return false;
        }
    }

    *body_len = offset;
    return true;
}

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
    uint32_t message_id;
    uint8_t packet_buf[PACKET_LEN] = {0};
    size_t packet_len = 0;
    mesh_packet_t packet = {0};

    message_id = mesh_sequence_next();
    mesh_sequence_save();
    packet.valid = true;
    packet.id = message_id;
    packet.hops = hops;
    packet.path_hops = 1;
    copy_field(packet.source, sizeof(packet.source), node_config_get_node_id());
    copy_field(packet.destination, sizeof(packet.destination), "ALL");
    copy_field(packet.type, sizeof(packet.type), "TIME_SYNC");
    copy_field(packet.priority, sizeof(packet.priority), "NORMAL");
    copy_field(packet.relay, sizeof(packet.relay), node_config_get_node_id());
    {
        char payload[64];
        snprintf(payload, sizeof(payload), "epoch=%lu~dist=%u", (unsigned long)epoch, distance);
        copy_field(packet.payload, sizeof(packet.payload), payload);
    }
    if (build_forward_packet_v2(&packet, packet_buf, sizeof(packet_buf), &packet_len)) {
        lora_transmit_bytes(packet_buf, packet_len);
    }
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

void mesh_control_send_sync_request(void)
{
    uint32_t request_id;
    uint8_t packet_buf[PACKET_LEN] = {0};
    size_t packet_len = 0;
    mesh_packet_t packet = {0};
    char payload[PAYLOAD_LEN] = {0};
    static emergency_message_t snapshot[MAX_MESSAGES];
    size_t count = message_store_copy_all(snapshot, MAX_MESSAGES);
    size_t offset = 0;
    uint8_t cursor_count = 0;

    request_id = mesh_sequence_next();
    mesh_sequence_save();
    payload[offset++] = 0; /* cursor count is filled after collecting unique sources */
    for (size_t i = 0; i < count && cursor_count < 8; i++) {
        const emergency_message_t *message = &snapshot[i];
        bool seen_source = false;
        uint32_t max_id = message->id;
        size_t source_len = strnlen(message->source, sizeof(message->source));
        for (size_t prior = 0; prior < i; prior++) {
            if (strcmp(snapshot[prior].source, message->source) == 0) {
                seen_source = true;
                break;
            }
        }
        if (seen_source || source_len == 0 || source_len >= FIELD_LEN) {
            continue;
        }
        for (size_t candidate = i + 1; candidate < count; candidate++) {
            if (strcmp(snapshot[candidate].source, message->source) == 0 && snapshot[candidate].id > max_id) {
                max_id = snapshot[candidate].id;
            }
        }
        if (offset + 1 + source_len + 4 > sizeof(payload)) {
            break;
        }
        payload[offset++] = (char)source_len;
        memcpy(&payload[offset], message->source, source_len);
        offset += source_len;
        payload[offset++] = (char)(max_id & 0xFF);
        payload[offset++] = (char)((max_id >> 8) & 0xFF);
        payload[offset++] = (char)((max_id >> 16) & 0xFF);
        payload[offset++] = (char)((max_id >> 24) & 0xFF);
        cursor_count++;
    }
    payload[0] = (char)cursor_count;
    packet.valid = true;
    packet.id = request_id;
    packet.hops = 1;
    packet.path_hops = 1;
    copy_field(packet.source, sizeof(packet.source), node_config_get_node_id());
    copy_field(packet.destination, sizeof(packet.destination), "ALL");
    copy_field(packet.type, sizeof(packet.type), "SYNC_REQ");
    copy_field(packet.priority, sizeof(packet.priority), "NORMAL");
    copy_field(packet.relay, sizeof(packet.relay), node_config_get_node_id());
    memcpy(packet.payload, payload, offset);
    packet.payload_len = offset;
    ESP_LOGI(TAG, "Broadcasting SYNC_REQ with %u source cursors", cursor_count);
    if (build_forward_packet_v2(&packet, packet_buf, sizeof(packet_buf), &packet_len)) {
        lora_transmit_bytes(packet_buf, packet_len);
    }
}

void mesh_control_send_boot_sync_request(void)
{
    mesh_control_send_sync_request();
}

void mesh_control_send_manual_sync_request(void)
{
    mesh_control_send_sync_request();
}

static uint32_t sync_cursor_for_source(const uint8_t *payload, size_t payload_len, const char *source)
{
    size_t offset = 0;
    uint8_t count;
    if (payload == NULL || payload_len < 1 || source == NULL) {
        return 0;
    }
    count = (uint8_t)payload[offset++];
    for (uint8_t i = 0; i < count; i++) {
        uint8_t source_len;
        uint32_t id;
        if (offset >= payload_len) return 0;
        source_len = (uint8_t)payload[offset++];
        if (source_len == 0 || offset + source_len + 4 > payload_len) return 0;
        bool matches = strlen(source) == source_len && memcmp(&payload[offset], source, source_len) == 0;
        offset += source_len;
        id = payload[offset] |
             ((uint32_t)payload[offset + 1] << 8) |
             ((uint32_t)payload[offset + 2] << 16) |
             ((uint32_t)payload[offset + 3] << 24);
        offset += 4;
        if (matches) return id;
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
        char *end = NULL;
        errno = 0;
        unsigned long distance = strtoul(dist_ptr + 6, &end, 10);
        if (end == dist_ptr + 6 || (*end != '\0' && *end != '~') || errno == ERANGE || distance > UINT8_MAX) return UINT8_MAX;
        return (uint8_t)distance;
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
    const uint32_t earliest_plausible_epoch = 1577836800u;
    const uint32_t latest_plausible_epoch = 4102444800u;

    if (dist == UINT8_MAX || epoch < earliest_plausible_epoch || epoch > latest_plausible_epoch) {
        return false;
    }

    if (!mesh_control_is_time_synced()) {
        mesh_control_apply_time_sync(epoch, dist);
        return true;
    }

    uint32_t local_epoch = mesh_control_current_epoch_seconds();
    uint32_t offset = epoch > local_epoch ? epoch - local_epoch : local_epoch - epoch;
    if (offset > 86400u) return false;
    if (dist < mesh_control_get_time_sync_distance()) mesh_control_apply_time_sync(epoch, dist);
    return true;
}

static bool is_private_destination_for_other_node(const char *destination, const char *requester)
{
    return strcmp(destination, "ALL") != 0 && strcmp(destination, requester) != 0;
}

bool mesh_control_build_ack(const mesh_packet_t *parsed, ack_info_t *ack_info)
{
    if (parsed == NULL || ack_info == NULL) {
        return false;
    }

    ack_info->acknowledged_id = parsed->id;
    copy_field(ack_info->acknowledged_source, sizeof(ack_info->acknowledged_source), parsed->source);
    return true;
}

bool mesh_control_parse_ack(const uint8_t *payload, size_t payload_len, ack_info_t *ack_info)
{
    if (payload == NULL || ack_info == NULL || payload_len < sizeof(uint32_t) + FIELD_LEN) {
        return false;
    }

    memcpy(&ack_info->acknowledged_id, payload, sizeof(uint32_t));
    memcpy(ack_info->acknowledged_source, payload + sizeof(uint32_t), FIELD_LEN);
    ack_info->acknowledged_source[FIELD_LEN - 1] = '\0';
    return true;
}

void mesh_control_send_ack_packet(const mesh_packet_t *parsed)
{
    uint32_t ack_id;
    mesh_packet_t packet = {0};
    ack_info_t ack_info = {0};

    ack_id = mesh_sequence_next();
    mesh_sequence_save();
    if (!mesh_control_build_ack(parsed, &ack_info)) {
        return;
    }
    packet.valid = true;
    packet.id = ack_id;
    packet.hops = parsed->hops > 0 ? parsed->hops : 3;
    packet.path_hops = 1;
    copy_field(packet.source, sizeof(packet.source), node_config_get_node_id());
    copy_field(packet.destination, sizeof(packet.destination), parsed->source);
    copy_field(packet.type, sizeof(packet.type), "ACK");
    copy_field(packet.priority, sizeof(packet.priority), "NORMAL");
    copy_field(packet.relay, sizeof(packet.relay), node_config_get_node_id());
    memcpy(packet.payload, &ack_info.acknowledged_id, sizeof(ack_info.acknowledged_id));
    memcpy(packet.payload + sizeof(ack_info.acknowledged_id), ack_info.acknowledged_source, FIELD_LEN);
    packet.payload_len = sizeof(ack_info.acknowledged_id) + FIELD_LEN;

    ESP_LOGI(TAG, "Sending ACK to %s for packet %lu", parsed->source, (unsigned long)parsed->id);
    route_entry_t route = {0};
    forward_job_t job = {0};
    copy_field(job.local_node_id, sizeof(job.local_node_id), node_config_get_node_id());
    job.packet = packet;
    job.route_known = route_table_get_best(packet.destination, &route);
    if (job.route_known) {
        job.route = route;
    }
    forward_worker_init();
    if (!forward_queue_enqueue(&job, TX_PRIORITY_HIGH)) {
        ESP_LOGW(TAG, "Forward queue full; dropping ACK for %s/%lu", parsed->source, (unsigned long)parsed->id);
    }
}

void mesh_control_send_sync_responses(const mesh_packet_t *request)
{
    static emergency_message_t snapshot[MAX_MESSAGES];
    size_t snapshot_count = 0;
    size_t stored_message_count;
    uint32_t last_id;

    stored_message_count = message_store_copy_all(snapshot, MAX_MESSAGES);
    for (size_t i = 0; i < stored_message_count; i++) {
        const emergency_message_t *message = &snapshot[i];

        if (strcmp(message->direction, "RX") != 0 && strcmp(message->direction, "TX") != 0) {
            continue;
        }
        last_id = sync_cursor_for_source((const uint8_t *)request->payload, request->payload_len, message->source);
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

    if (snapshot_count == 0) {
        return;
    }

    for (size_t start = 0; start < snapshot_count; ) {
        mesh_packet_t packet = {0};
        uint8_t body[PAYLOAD_LEN] = {0};
        uint8_t frame[PACKET_LEN] = {0};
        size_t body_len = 0;
        size_t frame_len = 0;
        size_t batch_start = start;
        size_t batch_count = 0;

        packet.valid = true;
        packet.id = mesh_sequence_next();
        packet.hops = 3;
        packet.path_hops = 1;
        copy_field(packet.source, sizeof(packet.source), node_config_get_node_id());
        copy_field(packet.destination, sizeof(packet.destination), request->source);
        copy_field(packet.type, sizeof(packet.type), "SYNC_RESP");
        copy_field(packet.priority, sizeof(packet.priority), "NORMAL");
        copy_field(packet.relay, sizeof(packet.relay), node_config_get_node_id());

        for (; start < snapshot_count; start++) {
            const emergency_message_t *message = &snapshot[start];
            size_t next_count = batch_count + 1;
            size_t next_body_len = 1;
            bool fits = true;

            if (next_count > 255) {
                break;
            }
            next_body_len += sync_response_record_encoded_size(message);
            for (size_t i = batch_start; i < start; i++) {
                next_body_len += sync_response_record_encoded_size(&snapshot[i]);
            }

            if (next_body_len > BEMS_MAX_PLAINTEXT || next_body_len > sizeof(body)) {
                if (batch_count == 0) {
                    ESP_LOGW(TAG, "Skipping oversized SYNC_RESP record for %s/%lu", message->source, (unsigned long)message->id);
                    start++;
                }
                fits = false;
            }

            if (!fits) {
                break;
            }

            batch_count = next_count;
        }

        if (batch_count == 0) {
            continue;
        }

        if (!mesh_control_encode_sync_response_batch(&snapshot[batch_start], batch_count, body, sizeof(body), &body_len)) {
            ESP_LOGW(TAG, "Skipping malformed SYNC_RESP batch");
            continue;
        }

        memcpy(packet.payload, body, body_len);
        packet.payload_len = body_len;
        if (!build_forward_packet_v2(&packet, frame, sizeof(frame), &frame_len)) {
            ESP_LOGW(TAG, "Skipping oversized SYNC_RESP batch");
            continue;
        }
        ESP_LOGI(TAG, "SYNC_RESP to %s with %u records", request->source, (unsigned int)batch_count);
        lora_transmit_bytes(frame, frame_len);
    }
}

static QueueHandle_t sync_response_queue;
static TaskHandle_t sync_response_task_handle;

static void sync_response_task(void *parameter)
{
    mesh_packet_t request;
    (void)parameter;
    while (true) {
        if (xQueueReceive(sync_response_queue, &request, portMAX_DELAY) == pdTRUE) {
            mesh_control_send_sync_responses(&request);
        }
    }
}

void mesh_control_enqueue_sync_response(const mesh_packet_t *request)
{
    if (request == NULL) return;
    if (sync_response_queue == NULL) {
        sync_response_queue = xQueueCreate(4, sizeof(mesh_packet_t));
        if (sync_response_queue == NULL) return;
    }
    if (sync_response_task_handle == NULL) {
        xTaskCreate(sync_response_task, "sync_response", 8192, NULL, 3, &sync_response_task_handle);
    }
    if (sync_response_task_handle != NULL && xQueueSend(sync_response_queue, request, 0) != pdTRUE) {
        ESP_LOGW(TAG, "SYNC response queue full; dropping request from %s", request->source);
    }
}

size_t mesh_control_decode_sync_response_records(const uint8_t *payload, size_t payload_len, mesh_packet_t *records, size_t max_records)
{
    size_t offset = 0;
    size_t count = 0;
    uint8_t record_count = 0;

    if (payload == NULL || records == NULL || max_records == 0 || payload_len < 1) {
        return 0;
    }

    record_count = payload[offset++];
    if (record_count == 0) {
        return 0;
    }

    for (uint8_t index = 0; index < record_count; index++) {
        uint32_t id = 0;
        int hops = 0;
        mesh_packet_t *record = &records[count];
        uint8_t source_len = 0;
        uint8_t destination_len = 0;
        uint8_t type_len = 0;
        uint8_t priority_len = 0;
        uint8_t payload_record_len = 0;

        if (count >= max_records || offset + SYNC_RESP_RECORD_FIXED_LEN > payload_len) {
            return 0;
        }
        memset(record, 0, sizeof(*record));
        id = (uint32_t)payload[offset] |
             ((uint32_t)payload[offset + 1] << 8) |
             ((uint32_t)payload[offset + 2] << 16) |
             ((uint32_t)payload[offset + 3] << 24);
        offset += 4;
        source_len = payload[offset++];
        if (source_len == 0 || offset + source_len > payload_len || source_len >= sizeof(record->source)) {
            return 0;
        }
        memcpy(record->source, &payload[offset], source_len);
        record->source[source_len] = '\0';
        offset += source_len;

        destination_len = payload[offset++];
        if (destination_len == 0 || offset + destination_len > payload_len) {
            return 0;
        }
        if (destination_len >= sizeof(record->destination)) {
            return 0;
        }
        memcpy(record->destination, &payload[offset], destination_len);
        record->destination[destination_len] = '\0';
        offset += destination_len;

        type_len = payload[offset++];
        if (type_len == 0 || offset + type_len > payload_len || type_len >= sizeof(record->type)) {
            return 0;
        }
        memcpy(record->type, &payload[offset], type_len);
        record->type[type_len] = '\0';
        offset += type_len;

        priority_len = payload[offset++];
        if (priority_len == 0 || offset + priority_len > payload_len || priority_len >= sizeof(record->priority)) {
            return 0;
        }
        memcpy(record->priority, &payload[offset], priority_len);
        record->priority[priority_len] = '\0';
        offset += priority_len;

        hops = (int)payload[offset++];
        payload_record_len = payload[offset++];
        if (offset + payload_record_len > payload_len || payload_record_len >= sizeof(record->payload)) {
            return 0;
        }
        memcpy(record->payload, &payload[offset], payload_record_len);
        record->payload[payload_record_len] = '\0';
        offset += payload_record_len;
        record->valid = true;
        record->id = id;
        record->hops = hops;
        record->broadcast_destination = strcmp(record->destination, "ALL") == 0;
        count++;
    }
    if (count != record_count || offset != payload_len) {
        return 0;
    }
    return count;
}
