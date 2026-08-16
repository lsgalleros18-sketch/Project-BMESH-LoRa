#include "app/app_runtime.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "esp_err.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_random.h"
#include "esp_system.h"
#include "mesh_protocol.h"
#include "mesh/replay_protection.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs.h"

#include "bems_crypto.h"
#include "http/http_auth.h"
#include "http/http_messages.h"
#include "http/http_portal.h"
#include "http/http_reset.h"
#include "http/http_send.h"
#include "http/http_setup.h"
#include "http/http_server.h"
#include "http/http_status.h"
#include "http/http_sync.h"
#include "http/http_time.h"
#include "led/status_led.h"
#include "mesh_control.h"
#include "mesh/mesh_sequence.h"
#include "mesh/mesh_retry.h"
#include "mesh/forward_queue.h"
#include "mesh/forward_worker.h"
#include "mesh/tx_scheduler.h"
#include "messages/message_store.h"
#include "network/dns_server.h"
#include "network/wifi_ap.h"
#include "node_config.h"
#include "radio/lora_radio.h"
#include "route_table.h"
#include "roster.h"
#include "storage/storage.h"
#include "system/factory_reset.h"
#include "utils/string_utils.h"

#define FIELD_LEN 32
#define SITIO_LEN 24
#define BARANGAY_LEN 24
#define MUNICIPALITY_LEN 24

static const char *TAG = "barangay_mesh";
static char node_id[FIELD_LEN];
static char ap_ssid[FIELD_LEN];
static bool duplicate_node_id_warning;
static bool littlefs_mounted;

static void time_sync_task(void *parameter);
static void boot_sync_task(void *parameter);
static tx_priority_t forward_priority_for_packet(const mesh_packet_t *packet);
static void store_received_packet(const char *packet, const mesh_packet_t *parsed, int rssi, int snr);
static void queue_message(const char *destination, const char *type, const char *priority, const char *payload);
static void start_http_server(void);

bool app_runtime_should_forward_packet(const mesh_packet_t *packet, const route_entry_t *route, const char *local_node_id)
{
    if (packet == NULL || local_node_id == NULL || local_node_id[0] == '\0') {
        return false;
    }
    if (packet->hops <= 0) {
        return false;
    }
    if (mesh_control_is_control_packet_type(packet->type)) {
        return false;
    }
    if (strcmp(packet->destination, local_node_id) == 0) {
        return false;
    }
    if (route == NULL || route->destination[0] == '\0' || route->stale || packet->next_hop[0] == '\0') {
        return true;
    }
    return strcmp(packet->next_hop, local_node_id) == 0;
}

static tx_priority_t forward_priority_for_packet(const mesh_packet_t *packet)
{
    if (packet == NULL) {
        return TX_PRIORITY_NORMAL;
    }
    if (strcmp(packet->priority, "HIGH") == 0) {
        return TX_PRIORITY_HIGH;
    }
    if (strcmp(packet->priority, "LOW") == 0) {
        return TX_PRIORITY_LOW;
    }
    return TX_PRIORITY_NORMAL;
}

static void boot_sync_task(void *parameter)
{
    vTaskDelay(pdMS_TO_TICKS(1500));

    if (node_config_get()->configured && lora_radio_is_ready()) {
        mesh_control_send_boot_sync_request();
    }

    vTaskDelete(NULL);
}

static void store_received_packet(const char *packet, const mesh_packet_t *parsed, int rssi, int snr)
{
    emergency_message_t message = {0};
    int storage_slot = -1;
    if (!message_store_allocate(&storage_slot)) {
        ESP_LOGW(TAG, "Dropping received packet because message table is full of active entries");
        return;
    }

    if (parsed->valid) {
        message.id = parsed->id;
    } else {
        message.id = mesh_sequence_next();
        mesh_sequence_save();
    }
    copy_field(message.direction, sizeof(message.direction), "RX");
    copy_field(message.source, sizeof(message.source), parsed->valid ? parsed->source : "UNKNOWN");
    copy_field(message.destination, sizeof(message.destination), parsed->valid ? parsed->destination : "UNKNOWN");
    copy_field(message.type, sizeof(message.type), parsed->valid ? parsed->type : "RECEIVED");
    copy_field(message.priority, sizeof(message.priority), parsed->valid ? parsed->priority : "NORMAL");
    copy_field(message.payload, sizeof(message.payload), parsed->valid ? parsed->payload : packet);
    if (parsed->valid) {
        message.origin_location = parsed->location;
        compute_thread_key(message.thread_key, sizeof(message.thread_key), parsed->source, parsed->destination);
        message.rssi = rssi;
        message.snr = snr;
        message.hops = parsed->hops;
    } else {
        copy_field(message.thread_key, sizeof(message.thread_key), "UNKNOWN");
        message.rssi = rssi;
        message.snr = snr;
        message.hops = 0;
    }
    copy_field(message.status, sizeof(message.status), "RECEIVED");
    message.stored_epoch = mesh_control_is_time_synced() ? mesh_control_current_epoch_seconds() : 0;
    snprintf(message.packet, sizeof(message.packet), "RSSI=%d SNR=%d | %.*s", rssi, snr, 250, packet);
    if (parsed->valid) {
        roster_touch(parsed->source, &parsed->location, message.stored_epoch, rssi, snr);
        route_table_learn(parsed->source, parsed->relay, parsed->hops, rssi);
    }
    if (parsed->valid) {
        bool is_relevant = (strcmp(parsed->source, node_id) == 0) ||
                          (strcmp(parsed->destination, node_id) == 0);
        if (is_relevant) {
            (void)message_store_write(storage_slot, &message);
        }
    }
}

void lora_handle_rx_packet(const uint8_t *payload, size_t length, int rssi, int snr)
{
    uint8_t decrypted_packet[PACKET_LEN];
    size_t decrypted_len = 0;
    bool is_v2;

    if (!bems_decrypt_frame_bytes(payload, length, decrypted_packet, sizeof(decrypted_packet), &decrypted_len)) {
        ESP_LOGW(TAG, "Rejected unauthenticated LoRa frame RSSI=%d SNR=%d length=%u", rssi, snr, (unsigned int)length);
        return;
    }

    is_v2 = mesh_packet_is_v2(decrypted_packet, decrypted_len);
    ESP_LOGI(TAG, "SX1278 RX RSSI=%d SNR=%d length=%u format=%s", rssi, snr, (unsigned int)decrypted_len, is_v2 ? "V2" : "V1");
    mesh_packet_t parsed;
    if ((is_v2 ? parse_mesh_packet_v2(decrypted_packet, decrypted_len, &parsed) : (decrypted_packet[decrypted_len] = 0, parse_mesh_packet((const char *)decrypted_packet, &parsed)))) {
        uint32_t now_ticks = xTaskGetTickCount();
        bool from_self = strcmp(parsed.source, node_id) == 0;
        bool replay_allowed = replay_protection_accept(parsed.source, parsed.id, now_ticks);
        bool is_broadcast = strcmp(parsed.destination, "ALL") == 0;
        bool is_for_me = strcmp(parsed.destination, node_id) == 0;
        bool is_ack = strcmp(parsed.type, "ACK") == 0;
        bool is_sync_req = strcmp(parsed.type, "SYNC_REQ") == 0;
        bool is_sync_resp = strcmp(parsed.type, "SYNC_RESP") == 0;
        route_entry_t route = {0};
        bool route_known = false;

        if (!from_self && replay_allowed) {
            remember_packet(parsed.source, parsed.id);
            if (is_sync_req) {
                mesh_control_send_sync_responses(&parsed);
                return;
            }
            if (is_sync_resp) {
                mesh_packet_t synced_packets[8];
                size_t synced_count = mesh_control_decode_sync_response_records((const uint8_t *)parsed.payload, parsed.payload_len, synced_packets, 8);
                for (size_t i = 0; i < synced_count; i++) {
                    if (replay_protection_accept(synced_packets[i].source, synced_packets[i].id, now_ticks)) {
                        store_received_packet(synced_packets[i].payload, &synced_packets[i], rssi, snr);
                    }
                }
                if (is_for_me && strcmp(parsed.source, node_id) != 0) {
                    duplicate_node_id_warning = true;
                }
                return;
            }
            if (strcmp(parsed.type, "TIME_SYNC") == 0) {
                (void)mesh_control_handle_time_sync_packet(parsed.payload);
                return;
            }
            if (is_broadcast || is_for_me) {
                store_received_packet((const char *)decrypted_packet, &parsed, rssi, snr);
            }
            if (is_for_me && !is_ack) {
                mesh_control_send_ack_packet(&parsed);
            }
            if (is_ack && is_for_me) {
                if (strcmp(parsed.source, node_id) != 0) {
                    duplicate_node_id_warning = true;
                }
                ack_info_t ack_info = {0};

                if (mesh_control_parse_ack((const uint8_t *)parsed.payload, parsed.payload_len, &ack_info) &&
                    ack_info.acknowledged_id != 0 &&
                    strcmp(ack_info.acknowledged_source, parsed.source) == 0 &&
                    strcmp(parsed.destination, node_id) == 0) {
                    (void)tx_scheduler_acknowledge(ack_info.acknowledged_id, parsed.source);
                }
            }
            if (!is_broadcast && !is_for_me && !mesh_control_is_control_packet_type(parsed.type)) {
                route_known = route_table_get_best(parsed.destination, &route);
            }
            if (parsed.hops > 0 && !is_for_me && !mesh_control_is_control_packet_type(parsed.type)) {
                forward_job_t job = {
                    .packet = parsed,
                    .rssi = rssi,
                    .snr = snr,
                    .route = route,
                    .route_known = route_known,
                };
                copy_field(job.local_node_id, sizeof(job.local_node_id), node_id);
                if (!forward_queue_enqueue(&job, forward_priority_for_packet(&parsed))) {
                    ESP_LOGW(TAG, "Forward queue full; dropping %s/%lu", parsed.source, (unsigned long)parsed.id);
                }
            }
        } else if (from_self) {
            mesh_packet_t raw_packet = {0};
            store_received_packet((const char *)decrypted_packet, &raw_packet, rssi, snr);
        }
    }
}

static void time_sync_task(void *parameter)
{
    while (true) {
        mesh_control_broadcast_time_sync_if_synced();
        vTaskDelay(pdMS_TO_TICKS(30000));
    }
}

static void queue_message(const char *destination, const char *type, const char *priority, const char *payload)
{
    emergency_message_t message = {0};
    int storage_slot = -1;

    message.id = mesh_sequence_next();
    copy_field(message.direction, sizeof(message.direction), "TX");
    copy_field(message.source, sizeof(message.source), node_id);
    copy_field(message.destination, sizeof(message.destination), destination);
    copy_field(message.type, sizeof(message.type), type);
    copy_field(message.priority, sizeof(message.priority), priority);
    copy_field(message.payload, sizeof(message.payload), payload);
    copy_field(message.status, sizeof(message.status), "PENDING");
    message.stored_epoch = mesh_control_is_time_synced() ? mesh_control_current_epoch_seconds() : 0;
    compute_thread_key(message.thread_key, sizeof(message.thread_key), message.source, message.destination);
    message.origin_location = node_config_get()->location;
    snprintf(message.packet, sizeof(message.packet), "TX %s -> %s type=%s priority=%s",
             message.source, message.destination, message.type, message.priority);
    mesh_sequence_save();

    if (!message_store_allocate(&storage_slot)) {
        ESP_LOGW(TAG, "Message queue full; unable to enqueue %s -> %s", node_id, destination);
        return;
    }
    (void)message_store_write(storage_slot, &message);

    if (!tx_scheduler_submit(&message, storage_slot)) {
        copy_field(message.status, sizeof(message.status), "FAILED");
        (void)message_store_update(storage_slot, &message);
        message_store_update_status(message.id, message.source, "FAILED");
        ESP_LOGW(TAG, "TX queue full; unable to enqueue %s -> %s", node_id, destination);
        return;
    }

    ESP_LOGI(TAG, "Queued LoRa TX: %s", message.packet);
}

static void start_http_server(void)
{
    static http_messages_context_t messages_context = {
        .require_session = http_auth_require_session,
    };
    static http_status_context_t status_context;
    static http_time_context_t time_context;
    static http_sync_context_t sync_context;
    static http_reset_context_t reset_context;
    static http_send_context_t send_context;
    static http_setup_context_t setup_context;
    status_context = (http_status_context_t){
        .require_session = http_auth_require_session,
        .current_epoch_seconds = mesh_control_current_epoch_seconds,
        .node_id = node_id,
        .node_name = node_config_get()->node_name,
        .node_role = node_config_get()->node_role,
        .location = node_config_get()->location.barangay,
        .ssid = ap_ssid,
        .configured = &node_config_get()->configured,
        .duplicate_node_id_warning = &duplicate_node_id_warning,
        .time_synced = mesh_control_time_synced_ref(),
    };
    time_context = (http_time_context_t){
        .apply_time_sync = mesh_control_apply_time_sync,
        .send_time_sync_packet = mesh_control_send_time_sync_packet,
    };
    sync_context = (http_sync_context_t){
        .send_manual_sync_request = mesh_control_send_manual_sync_request,
    };
    reset_context = (http_reset_context_t){
        .erase_node_config = node_config_erase,
    };
    send_context = (http_send_context_t){
        .default_destination = node_config_get_default_destination(),
        .queue_message = queue_message,
    };
    setup_context = (http_setup_context_t){
        .node_id = node_id,
        .ap_password = node_config_get_ap_password(),
        .default_web_pin = node_config_get_web_pin(),
        .default_network_key = node_config_get_network_key(),
        .copy_node_id = node_config_copy_node_id,
        .save_node_config = node_config_save,
    };

    http_server_start(&messages_context,
                      &status_context,
                      &time_context,
                      &sync_context,
                      &reset_context,
                      &send_context,
                      &setup_context);
}

void app_runtime_start(void)
{
    {
        uint8_t mac[6];
        ESP_ERROR_CHECK(esp_read_mac(mac, ESP_MAC_WIFI_SOFTAP));
        snprintf(node_id, sizeof(node_id), "NODE%02X%02X", mac[4], mac[5]);
        snprintf(ap_ssid, sizeof(ap_ssid), "BarangayMesh-SETUP-%02X%02X", mac[4], mac[5]);
    }
    node_config_set_identity(node_id);
    http_portal_init(&(http_portal_context_t){
        .configured = &node_config_get()->configured,
        .littlefs_mounted = &littlefs_mounted,
        .littlefs_base_path = "/littlefs",
    });
    http_auth_init(&(http_auth_context_t){
        .configured = &node_config_get()->configured,
        .web_pin = node_config_get()->web_pin,
        .send_portal_file = http_portal_send_file,
    });
    mesh_sequence_init();
    replay_protection_reset();
    status_led_init();
    factory_reset_init();
    node_config_load();
    message_store_load_messages_from_nvs(node_id);
    (void)message_store_init();
    forward_worker_init();
    lora_radio_init();
    storage_init(&littlefs_mounted);
    tx_scheduler_init();
    xTaskCreate(boot_sync_task, "boot_sync_task", 4096, NULL, 4, NULL);
    mesh_retry_init();
    xTaskCreate(time_sync_task, "time_sync_task", 3072, NULL, 2, NULL);
    wifi_ap_init(ap_ssid, node_config_get()->ap_password);
    start_http_server();
    dns_server_init();

    ESP_LOGI(TAG, "Universal mesh node portal ready as %s", node_id);
}
