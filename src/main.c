#include <ctype.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/param.h>

#include "driver/gpio.h"
#include "esp_event.h"
#include "esp_littlefs.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_random.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "lwip/inet.h"
#include "lwip/sockets.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "psa/crypto.h"

#include "bems_crypto.h"
#include "app/app_init.h"
#include "http/http_auth.h"
#include "http/http_messages.h"
#include "http/http_portal.h"
#include "http/http_reset.h"
#include "http/http_send.h"
#include "http/http_setup.h"
#include "http/http_server.h"
#include "http/http_status.h"
#include "http/http_time.h"
#include "http/http_sync.h"
#include "network/dns_server.h"
#include "network/wifi_ap.h"
#include "roster.h"
#include "route_table.h"
#include "messages/message_store.h"
#include "node_config.h"
#include "json/json_writer.h"
#include "mesh_control.h"
#include "mesh/mesh_retry.h"
#include "led/status_led.h"
#include "radio/lora_radio.h"
#include "storage/storage.h"
#include "system/factory_reset.h"
#include "utils/json_utils.h"
#include "utils/string_utils.h"

#define FIELD_LEN 32
#define SITIO_LEN 24
#define BARANGAY_LEN 24
#define MUNICIPALITY_LEN 24
#define CONFIG_NAMESPACE "bems_config"
#define PACKET_COUNTER_KEY "packet_ctr"
#define DEFAULT_WEB_PIN "123456789"
#define DEFAULT_NETWORK_KEY "CHANGEME1234567"

void location_encode(const location_info_t *loc, char *out, size_t out_size);
void location_decode(const char *encoded, location_info_t *loc);
static void time_sync_task(void *parameter);

static const char *TAG = "barangay_mesh";
static char node_id[FIELD_LEN];
static char ap_ssid[FIELD_LEN];
static uint32_t packet_counter;
static bool duplicate_node_id_warning;
static bool littlefs_mounted;
static SemaphoreHandle_t data_mutex;
static void save_packet_counter(void);
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

typedef struct {
    mesh_packet_t packet;
    int rssi;
    int snr;
} forward_packet_task_args_t;

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
    int nvs_slot;
    data_lock();
    emergency_message_t *message = message_store_begin_write(&nvs_slot);
    bool counter_changed = false;

    if (message == NULL) {
        message_store_end_update();
        data_unlock();
        ESP_LOGW(TAG, "Dropping received packet because message table is full of active entries");
        return;
    }

    if (parsed->valid) {
        message->id = parsed->id;
    } else {
        message->id = ++packet_counter;
        counter_changed = true;
    }
    copy_field(message->direction, sizeof(message->direction), "RX");
    copy_field(message->source, sizeof(message->source), parsed->valid ? parsed->source : "UNKNOWN");
    copy_field(message->destination, sizeof(message->destination), parsed->valid ? parsed->destination : "UNKNOWN");
    copy_field(message->type, sizeof(message->type), parsed->valid ? parsed->type : "RECEIVED");
    copy_field(message->priority, sizeof(message->priority), parsed->valid ? parsed->priority : "NORMAL");
    copy_field(message->payload, sizeof(message->payload), parsed->valid ? parsed->payload : packet);
    if (parsed->valid) {
        message->origin_location = parsed->location;
        compute_thread_key(message->thread_key, sizeof(message->thread_key), parsed->source, parsed->destination);
        message->rssi = rssi;
        message->snr = snr;
        message->hops = parsed->hops;
    } else {
        copy_field(message->thread_key, sizeof(message->thread_key), "UNKNOWN");
        message->rssi = rssi;
        message->snr = snr;
        message->hops = 0;
    }
    copy_field(message->status, sizeof(message->status), parsed->valid ? "RECEIVED" : "RECEIVED");
    message->stored_epoch = mesh_control_is_time_synced() ? mesh_control_current_epoch_seconds() : 0;
    snprintf(message->packet, sizeof(message->packet), "RSSI=%d SNR=%d | %.*s", rssi, snr, 250, packet);
    if (parsed->valid && !mesh_control_is_control_packet_type(parsed->type)) {
        mesh_control_update_highest_seen_id(message->id);
    }
    if (parsed->valid) {
        roster_touch(parsed->source, &parsed->location, message->stored_epoch, rssi, snr);
        route_table_learn(parsed->source, parsed->hops, rssi);
    }
    if (counter_changed) {
        save_packet_counter();
    }
    // Only persist messages where this node is source or destination (skip pure relay traffic)
    if (parsed->valid) {
        bool is_relevant = (strcmp(parsed->source, node_id) == 0) || 
                          (strcmp(parsed->destination, node_id) == 0);
        if (is_relevant) {
            message_store_save_message_to_nvs(message, nvs_slot);
        }
    }
    message_store_end_update();
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
        bool from_self = strcmp(parsed.source, node_id) == 0;
        bool is_duplicate = packet_seen(parsed.source, parsed.id);
        bool is_broadcast = strcmp(parsed.destination, "ALL") == 0;
        bool is_for_me = strcmp(parsed.destination, node_id) == 0;
        bool is_ack = strcmp(parsed.type, "ACK") == 0;
        bool is_sync_req = strcmp(parsed.type, "SYNC_REQ") == 0;
        bool is_sync_resp = strcmp(parsed.type, "SYNC_RESP") == 0;

        if (!from_self && !is_duplicate) {
            remember_packet(parsed.source, parsed.id);

            if (is_sync_req) {
                mesh_control_send_sync_responses(&parsed);
                return;
            }

            if (is_sync_resp) {
                mesh_packet_t synced_packet;

                if (is_for_me && parse_mesh_packet(parsed.payload, &synced_packet) && !packet_seen(synced_packet.source, synced_packet.id)) {
                    remember_packet(synced_packet.source, synced_packet.id);
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
                    message_store_update_status(ack_id, node_id, "ACKED");
                }
            }

            if (parsed.hops > 0 && !is_for_me && !mesh_control_is_control_packet_type(parsed.type)) {
                forward_packet_task_args_t *forward_args = (forward_packet_task_args_t *)pvPortMalloc(sizeof(*forward_args));

                if (forward_args != NULL) {
                    forward_args->packet = parsed;
                    forward_args->rssi = rssi;
                    forward_args->snr = snr;
                    xTaskCreate(delayed_forward_task, "fwd_pkt", 4096, forward_args, 5, NULL);
                } else {
                    ESP_LOGW(TAG, "Failed to allocate delayed forward args for %s/%lu", parsed.source, (unsigned long)parsed.id);
                }
            }
        } else {
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

static void load_packet_counter(void)
{
    nvs_handle_t handle;
    uint32_t stored_counter = 0;

    if (nvs_open(CONFIG_NAMESPACE, NVS_READONLY, &handle) != ESP_OK) {
        ESP_LOGI(TAG, "No saved packet counter; starting at %lu", (unsigned long)packet_counter);
        return;
    }

    if (nvs_get_u32(handle, PACKET_COUNTER_KEY, &stored_counter) == ESP_OK) {
        packet_counter = stored_counter;
        ESP_LOGI(TAG, "Packet counter restored: %lu", (unsigned long)packet_counter);
    } else {
        ESP_LOGI(TAG, "No saved packet counter; starting at %lu", (unsigned long)packet_counter);
    }

    nvs_close(handle);
}

static void save_packet_counter(void)
{
    nvs_handle_t handle;
    esp_err_t result = nvs_open(CONFIG_NAMESPACE, NVS_READWRITE, &handle);

    if (result != ESP_OK) {
        ESP_LOGW(TAG, "Failed to open NVS for packet counter: %s", esp_err_to_name(result));
        return;
    }

    result = nvs_set_u32(handle, PACKET_COUNTER_KEY, packet_counter);
    if (result == ESP_OK) {
        result = nvs_commit(handle);
    }

    if (result != ESP_OK) {
        ESP_LOGW(TAG, "Failed to save packet counter: %s", esp_err_to_name(result));
    }

    nvs_close(handle);
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

    written = snprintf(message->packet + offset, sizeof(message->packet) - offset, "BEMS|%lu|%.*s|%.*s|%.*s|%.*s|HOPS=%d|RELAY=%u|LOC=%s|",
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
                       1,
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
    uint32_t queued_id;
    char queued_source[FIELD_LEN];
    char queued_destination[FIELD_LEN];
    char queued_priority[FIELD_LEN];
    int nvs_slot;

    data_lock();
    emergency_message_t *message = message_store_begin_write(&nvs_slot);

    if (message == NULL) {
        message_store_end_update();
        data_unlock();
        ESP_LOGW(TAG, "Message queue full; unable to enqueue %s -> %s", node_id, destination);
        return;
    }

    message->id = ++packet_counter;
    copy_field(message->direction, sizeof(message->direction), "TX");
    copy_field(message->source, sizeof(message->source), node_id);
    copy_field(message->destination, sizeof(message->destination), destination);
    copy_field(message->type, sizeof(message->type), type);
    copy_field(message->priority, sizeof(message->priority), priority);
    copy_field(message->payload, sizeof(message->payload), payload);
    copy_field(message->status, sizeof(message->status), "PENDING");
    message->stored_epoch = mesh_control_is_time_synced() ? mesh_control_current_epoch_seconds() : 0;
    build_packet(message);
    compute_thread_key(message->thread_key, sizeof(message->thread_key), message->source, message->destination);
    message->origin_location = node_config_get()->location;
    queued_id = message->id;
    copy_field(queued_source, sizeof(queued_source), message->source);
    copy_field(queued_destination, sizeof(queued_destination), message->destination);
    copy_field(queued_priority, sizeof(queued_priority), message->priority);
    copy_field(packet, sizeof(packet), message->packet);
    save_packet_counter();
    message_store_save_message_to_nvs(message, nvs_slot);
    message_store_end_update();
    data_unlock();

    ESP_LOGI(TAG, "LoRa TX pending: %s", packet);
    if (lora_transmit(packet)) {
        message_store_update_status(queued_id, queued_source, "SENT");
        mesh_retry_track(queued_id, queued_source, queued_destination, queued_priority);
    } else {
        message_store_update_status(queued_id, queued_source, "FAILED");
    }
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

void app_main(void)
{
    ESP_ERROR_CHECK(app_init());

    data_mutex = xSemaphoreCreateMutex();
    ESP_ERROR_CHECK(data_mutex == NULL ? ESP_ERR_NO_MEM : ESP_OK);

    packet_counter = esp_random() & 0xFFFF;
    load_packet_counter();

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
    mesh_control_load_highest_seen_id();
    status_led_init();
    factory_reset_init();
    node_config_load();
    message_store_load_messages_from_nvs(node_id);
    lora_radio_init();
    storage_init(&littlefs_mounted);
    xTaskCreate(boot_sync_task, "boot_sync_task", 4096, NULL, 4, NULL);
    mesh_retry_init();
    xTaskCreate(time_sync_task, "time_sync_task", 3072, NULL, 2, NULL);
    wifi_ap_init(ap_ssid, node_config_get()->ap_password);
    start_http_server();
    dns_server_init();

    ESP_LOGI(TAG, "Universal mesh node portal ready as %s", node_id);
}



