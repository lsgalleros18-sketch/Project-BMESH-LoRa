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
#include "esp_timer.h"
#include "mesh_protocol.h"
#include "mesh/replay_protection.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
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
static SemaphoreHandle_t data_mutex;

static void time_sync_task(void *parameter);
static void boot_sync_task(void *parameter);
static void data_lock(void);
static void data_unlock(void);
static uint32_t ack_id_from_payload(const char *payload);
static int hops_for_priority(const char *priority);
static void build_packet(emergency_message_t *message);
static void store_received_packet(const char *packet, const mesh_packet_t *parsed, int rssi, int snr);
static void delayed_forward_task(void *parameter);
static void queue_message(const char *destination, const char *type, const char *priority, const char *payload);
static void start_http_server(void);

typedef struct {
    mesh_packet_t packet;
    int rssi;
    int snr;
    bool route_known;
    route_entry_t route;
} forward_packet_task_args_t;

static void data_lock(void)
{
    if (data_mutex != NULL) {
        xSemaphoreTake(data_mutex, portMAX_DELAY);
    }
}

static void data_unlock(void)
{
    if (data_mutex != NULL) {
        xSemaphoreGive(data_mutex);
    }
}

static uint32_t ack_id_from_payload(const char *payload)
{
    const char *marker = strrchr(payload, ' ');

    if (marker == NULL) {
        return 0;
    }

    return (uint32_t)strtoul(marker + 1, NULL, 10);
}

static void delayed_forward_task(void *parameter)
{
    forward_packet_task_args_t *args = (forward_packet_task_args_t *)parameter;
    char forward_packet[PACKET_LEN];
    uint32_t delay_ms = 100 + (esp_random() % 500);

    vTaskDelay(pdMS_TO_TICKS(delay_ms));

    if (packet_seen(args->packet.source, args->packet.id)) {
        ESP_LOGI(TAG, "Suppressed duplicate forward for %s/%lu after %u ms", args->packet.source, (unsigned long)args->packet.id, (unsigned int)delay_ms);
        vPortFree(args);
        vTaskDelete(NULL);
    }

    if (args->packet.hops > 0) {
        if (args->route_known) {
            ESP_LOGI(TAG, "Route-selected forward for %s -> %s via %s (hop=%d rssi=%d age=%lu ms)",
                     args->packet.source,
                     args->packet.destination,
                     args->route.next_hop,
                     args->route.hop_count,
                     args->route.best_rssi,
                     (unsigned long)(esp_timer_get_time() / 1000ULL - args->route.last_seen_tick_ms));
        } else {
            ESP_LOGI(TAG, "Flood fallback forward for %s -> %s", args->packet.source, args->packet.destination);
        }
        copy_field(args->packet.relay, sizeof(args->packet.relay), node_id);
        build_forward_packet(&args->packet, forward_packet, sizeof(forward_packet));
        ESP_LOGI(TAG, "Forwarding packet %s/%lu after %u ms delay", args->packet.source, (unsigned long)args->packet.id, (unsigned int)delay_ms);
        lora_transmit(forward_packet);
    }

    vPortFree(args);
    vTaskDelete(NULL);
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
    int nvs_slot = -1;
    data_lock();

    if (!message_store_add(&message, &nvs_slot)) {
        data_unlock();
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
            message_store_save_message_to_nvs(&message, nvs_slot);
        }
    }
    data_unlock();
}

void lora_handle_rx_packet(const uint8_t *payload, size_t length, int rssi, int snr)
{
    char decrypted_packet[PACKET_LEN];
    if (!bems_decrypt_frame(payload, length, decrypted_packet, sizeof(decrypted_packet))) {
        ESP_LOGW(TAG, "Rejected unauthenticated LoRa frame RSSI=%d SNR=%d length=%u", rssi, snr, (unsigned int)length);
        return;
    }

    ESP_LOGI(TAG, "SX1278 RX RSSI=%d SNR=%d: %s", rssi, snr, decrypted_packet);
    mesh_packet_t parsed;
    if (parse_mesh_packet(decrypted_packet, &parsed)) {
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
                mesh_packet_t synced_packet;

                if (is_for_me && parse_mesh_packet(parsed.payload, &synced_packet) && replay_protection_accept(synced_packet.source, synced_packet.id, now_ticks)) {
                    store_received_packet(parsed.payload, &synced_packet, rssi, snr);
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
                store_received_packet(decrypted_packet, &parsed, rssi, snr);
            }

            if (is_for_me && !is_ack) {
                mesh_control_send_ack_packet(&parsed);
            }

            if (is_ack && is_for_me) {
                if (strcmp(parsed.source, node_id) != 0) {
                    duplicate_node_id_warning = true;
                }
                uint32_t ack_id = ack_id_from_payload(parsed.payload);
                if (ack_id != 0) {
                    (void)tx_scheduler_acknowledge(ack_id, parsed.source);
                }
            }

            if (!is_broadcast && !is_for_me && !mesh_control_is_control_packet_type(parsed.type)) {
                route_known = route_table_get_best(parsed.destination, &route);
            }

            if (parsed.hops > 0 && !is_for_me && !mesh_control_is_control_packet_type(parsed.type)) {
                forward_packet_task_args_t *forward_args = (forward_packet_task_args_t *)pvPortMalloc(sizeof(*forward_args));

                if (forward_args != NULL) {
                    forward_args->packet = parsed;
                    forward_args->rssi = rssi;
                    forward_args->snr = snr;
                    forward_args->route_known = route_known;
                    if (route_known) {
                        forward_args->route = route;
                    } else {
                        memset(&forward_args->route, 0, sizeof(forward_args->route));
                    }
                    xTaskCreate(delayed_forward_task, "fwd_pkt", 4096, forward_args, 5, NULL);
                } else {
                    ESP_LOGW(TAG, "Failed to allocate delayed forward args for %s/%lu", parsed.source, (unsigned long)parsed.id);
                }
            }
        } else if (from_self) {
            mesh_packet_t raw_packet = {0};
            store_received_packet(decrypted_packet, &raw_packet, rssi, snr);
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

static int hops_for_priority(const char *priority)
{
    if (strcmp(priority, "HIGH") == 0) {
        return 5;
    }
    if (strcmp(priority, "LOW") == 0) {
        return 1;
    }

    return 3;
}

static void build_packet(emergency_message_t *message)
{
    char encoded_location[SITIO_LEN + BARANGAY_LEN + MUNICIPALITY_LEN + 2];
    size_t offset = 0;
    int written;

    location_encode(&node_config_get()->location, encoded_location, sizeof(encoded_location));

    written = snprintf(message->packet + offset, sizeof(message->packet) - offset, "BEMS|%lu|%.*s|%.*s|%.*s|%.*s|HOPS=%d|RELAY=%.*s|LOC=%s|",
                       (unsigned long)message->id,
                       31,
                       node_id,
                       31,
                       message->destination,
                       31,
                       message->type,
                       31,
                       message->priority,
                       hops_for_priority(message->priority),
                       31,
                       node_id,
                       encoded_location);
    if (written < 0 || (size_t)written >= sizeof(message->packet) - offset) {
        message->packet[0] = '\0';
        return;
    }
    offset += (size_t)written;

    written = snprintf(message->packet + offset, sizeof(message->packet) - offset, "%.*s", 120, message->payload);
    if (written < 0 || (size_t)written >= sizeof(message->packet) - offset) {
        message->packet[sizeof(message->packet) - 1] = '\0';
        return;
    }
}

static void queue_message(const char *destination, const char *type, const char *priority, const char *payload)
{
    char packet[PACKET_LEN];
    emergency_message_t message = {0};
    int nvs_slot = -1;

    message.id = mesh_sequence_next();
    copy_field(message.direction, sizeof(message.direction), "TX");
    copy_field(message.source, sizeof(message.source), node_id);
    copy_field(message.destination, sizeof(message.destination), destination);
    copy_field(message.type, sizeof(message.type), type);
    copy_field(message.priority, sizeof(message.priority), priority);
    copy_field(message.payload, sizeof(message.payload), payload);
    copy_field(message.status, sizeof(message.status), "PENDING");
    message.stored_epoch = mesh_control_is_time_synced() ? mesh_control_current_epoch_seconds() : 0;
    build_packet(&message);
    compute_thread_key(message.thread_key, sizeof(message.thread_key), message.source, message.destination);
    message.origin_location = node_config_get()->location;
    copy_field(packet, sizeof(packet), message.packet);
    mesh_sequence_save();

    data_lock();
    if (!message_store_add(&message, &nvs_slot)) {
        data_unlock();
        ESP_LOGW(TAG, "Message queue full; unable to enqueue %s -> %s", node_id, destination);
        return;
    }
    message_store_save_message_to_nvs(&message, nvs_slot);
    data_unlock();

    ESP_LOGI(TAG, "Queued LoRa TX: %s", packet);
    (void)tx_scheduler_enqueue(&message, nvs_slot);
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
    data_mutex = xSemaphoreCreateMutex();
    ESP_ERROR_CHECK(data_mutex == NULL ? ESP_ERR_NO_MEM : ESP_OK);

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
