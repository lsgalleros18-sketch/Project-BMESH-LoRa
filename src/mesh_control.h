#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "mesh_protocol.h"
#include "messages/message_store.h"

#define MESH_CONTROL_HIGHEST_SEEN_ID_KEY "highest_seen"

void mesh_control_load_highest_seen_id(void);
uint32_t mesh_control_get_highest_seen_id(void);
void mesh_control_update_highest_seen_id(uint32_t id);

uint32_t mesh_control_current_epoch_seconds(void);
void mesh_control_apply_time_sync(uint32_t epoch, uint8_t distance);
bool mesh_control_is_time_synced(void);
const bool *mesh_control_time_synced_ref(void);
uint8_t mesh_control_get_time_sync_distance(void);
void mesh_control_send_time_sync_packet(uint32_t epoch, uint8_t distance, uint8_t hops);
void mesh_control_broadcast_time_sync_if_synced(void);
void mesh_control_send_sync_request(uint32_t last_id);
void mesh_control_send_boot_sync_request(void);
void mesh_control_send_manual_sync_request(void);
bool mesh_control_is_control_packet_type(const char *type);
bool mesh_control_handle_time_sync_packet(const char *payload);
typedef struct {
    uint32_t acknowledged_id;
    char acknowledged_source[FIELD_LEN];
} ack_info_t;

bool mesh_control_build_ack(const mesh_packet_t *parsed, ack_info_t *ack_info);
bool mesh_control_parse_ack(const uint8_t *payload, size_t payload_len, ack_info_t *ack_info);
void mesh_control_send_ack_packet(const mesh_packet_t *parsed);
void mesh_control_send_sync_responses(const mesh_packet_t *request);
size_t mesh_control_decode_sync_response_records(const uint8_t *payload, size_t payload_len, mesh_packet_t *records, size_t max_records);
bool mesh_control_encode_sync_response_batch(const emergency_message_t *records, size_t record_count, uint8_t *payload, size_t payload_size, size_t *payload_len);
