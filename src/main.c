#include <ctype.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/param.h>

#include "driver/gpio.h"
#include "driver/rmt_tx.h"
#include "driver/spi_master.h"
#include "esp_event.h"
#include "esp_http_server.h"
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
#include "http/http_auth.h"
#include "http/http_messages.h"
#include "http/http_portal.h"
#include "http/http_status.h"
#include "http/http_time.h"
#include "http/http_sync.h"
#include "roster.h"
#include "route_table.h"
#include "messages/message_store.h"
#include "json/json_writer.h"
#include "utils/json_utils.h"
#include "utils/string_utils.h"

#define AP_CHANNEL 6
#define AP_MAX_CONNECTIONS 4
#define DNS_PORT 53
#define HTTP_PORT 80
#define MAX_SEEN_PACKETS 64
#define SEEN_PACKET_TTL_MS 60000
#define FIELD_LEN 32
#define SITIO_LEN 24
#define BARANGAY_LEN 24
#define MUNICIPALITY_LEN 24
#define BOOT_BUTTON_GPIO 0
#define RGB_LED_GPIO 48
#define FACTORY_RESET_HOLD_MS 10000
#define RESET_WARNING_MS 5000
#define CONFIG_NAMESPACE "bems_config"
#define PACKET_COUNTER_KEY "packet_ctr"
#define HIGHEST_SEEN_ID_KEY "highest_seen"
#define DEFAULT_WEB_PIN "123456789"
#define DEFAULT_NETWORK_KEY "CHANGEME1234567"
#define LITTLEFS_BASE_PATH "/littlefs"

#define LORA_MISO_GPIO 5
#define LORA_DIO0_GPIO 16
#define LORA_SCK_GPIO 7
#define LORA_MOSI_GPIO 6
#define LORA_RST_GPIO 4
#define LORA_NSS_GPIO 8
#define LORA_SPI_HOST SPI2_HOST
#define LORA_FREQUENCY_HZ 433000000UL
#define LORA_MAX_PAYLOAD 255

#define REG_FIFO 0x00
#define REG_OP_MODE 0x01
#define REG_FRF_MSB 0x06
#define REG_FRF_MID 0x07
#define REG_FRF_LSB 0x08
#define REG_PA_CONFIG 0x09
#define REG_LNA 0x0C
#define REG_FIFO_ADDR_PTR 0x0D
#define REG_FIFO_TX_BASE_ADDR 0x0E
#define REG_FIFO_RX_BASE_ADDR 0x0F
#define REG_FIFO_RX_CURRENT_ADDR 0x10
#define REG_IRQ_FLAGS 0x12
#define REG_RX_NB_BYTES 0x13
#define REG_PKT_SNR_VALUE 0x19
#define REG_PKT_RSSI_VALUE 0x1A
#define REG_MODEM_CONFIG_1 0x1D
#define REG_MODEM_CONFIG_2 0x1E
#define REG_PREAMBLE_MSB 0x20
#define REG_PREAMBLE_LSB 0x21
#define REG_PAYLOAD_LENGTH 0x22
#define REG_MODEM_CONFIG_3 0x26
#define REG_SYNC_WORD 0x39
#define REG_DIO_MAPPING_1 0x40
#define REG_IRQ_FLAGS_1 0x3E
#define REG_VERSION 0x42

#define MODE_LONG_RANGE_MODE 0x80
#define MODE_SLEEP 0x00
#define MODE_STDBY 0x01
#define MODE_TX 0x03
#define MODE_RX_CONTINUOUS 0x05

#define IRQ_TX_DONE_MASK 0x08
#define IRQ_PAYLOAD_CRC_ERROR_MASK 0x20
#define IRQ_RX_DONE_MASK 0x40

#define IRQ1_CAD_DONE_MASK 0x04
#define IRQ1_CAD_DETECTED_MASK 0x01

#define LORA_BW_125_KHZ 0x70
#define LORA_CR_4_5 0x02
#define LORA_EXPLICIT_HEADER_MODE 0x00
#define LORA_SPREADING_FACTOR 7
#define LORA_TX_CONTINUOUS_MODE 0x00
#define LORA_RX_PAYLOAD_CRC_ON 0x04
#define LORA_LOW_DATA_RATE_OPTIMIZE_OFF 0x00
#define LORA_AGC_AUTO_ON 0x04
#define LORA_MODEM_CONFIG_1 (LORA_BW_125_KHZ | LORA_CR_4_5 | LORA_EXPLICIT_HEADER_MODE)
#define LORA_MODEM_CONFIG_2 ((LORA_SPREADING_FACTOR << 4) | LORA_TX_CONTINUOUS_MODE | LORA_RX_PAYLOAD_CRC_ON)
#define LORA_MODEM_CONFIG_3 (LORA_LOW_DATA_RATE_OPTIMIZE_OFF | LORA_AGC_AUTO_ON)

typedef struct {
    bool configured;
    char node_id[FIELD_LEN];
    char node_name[FIELD_LEN];
    char node_role[FIELD_LEN];
    location_info_t location;          // was: char location[FIELD_LEN]
    char default_destination[FIELD_LEN];
    char default_priority[FIELD_LEN];
    char ap_password[FIELD_LEN];
    char web_pin[FIELD_LEN];
    char duress_pin[FIELD_LEN];
    char network_key[FIELD_LEN];
} node_config_t;

void location_encode(const location_info_t *loc, char *out, size_t out_size);
void location_decode(const char *encoded, location_info_t *loc);
static void update_message_status(uint32_t id, const char *source, const char *status);
static bool lora_channel_clear(void);
static void retry_tracker_task(void *parameter);
static void time_sync_task(void *parameter);
static uint32_t current_epoch_seconds(void);
static void apply_time_sync(uint32_t epoch, uint8_t distance);
static void send_time_sync_packet(uint32_t epoch, uint8_t distance, uint8_t hops);
static void broadcast_time_sync_if_synced(void);

typedef struct {
    uint32_t id;
    TickType_t seen_tick;
    char source[FIELD_LEN];
} seen_packet_t;

typedef struct {
    bool valid;
    uint32_t id;
    int hops;
    char source[FIELD_LEN];
    char destination[FIELD_LEN];
    char type[FIELD_LEN];
    char priority[FIELD_LEN];
    char relay[FIELD_LEN];
    char location_raw[PACKET_LEN];
    location_info_t location; // was: char location[FIELD_LEN]
    char payload[PAYLOAD_LEN];
} mesh_packet_t;

typedef struct {
    uint32_t id;
    char source[FIELD_LEN];
    char destination[FIELD_LEN];
    char priority[FIELD_LEN];
    uint8_t attempts;
    TickType_t next_retry_tick;
    bool active;
} retry_entry_t;

static const char *TAG = "barangay_mesh";
static const char *AP_PASSWORD = "123456789";

static char node_id[FIELD_LEN];
static char ap_ssid[FIELD_LEN];
static node_config_t node_config;
static seen_packet_t seen_packets[MAX_SEEN_PACKETS];
static retry_entry_t retry_entries[MAX_MESSAGES];
static size_t seen_packet_count;
static uint32_t packet_counter;
static uint32_t highest_seen_id;
static TickType_t last_send_tick;
static int64_t epoch_offset_sec;
static bool time_synced;
static uint8_t time_sync_distance;
static TickType_t last_time_sync_broadcast_tick;
static httpd_handle_t http_server;
static spi_device_handle_t lora_spi;
static rmt_channel_handle_t rgb_led_channel;
static rmt_encoder_handle_t rgb_led_encoder;
static bool rgb_led_ready;
static bool lora_ready;
static volatile bool radio_in_tx;
static bool duplicate_node_id_warning;
static bool littlefs_mounted;
static SemaphoreHandle_t lora_dio0_semaphore;
static SemaphoreHandle_t lora_tx_done_semaphore;
static SemaphoreHandle_t data_mutex;
static void save_packet_counter(void);
static void update_highest_seen_id(uint32_t id);
static bool lora_transmit(const char *packet);
static esp_err_t save_node_config(const node_config_t *config);

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

static void rgb_led_init(void)
{
    rmt_tx_channel_config_t channel_config = {
        .gpio_num = RGB_LED_GPIO,
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .resolution_hz = 10000000,
        .mem_block_symbols = 64,
        .trans_queue_depth = 4,
    };
    rmt_bytes_encoder_config_t encoder_config = {
        .bit0 = {
            .level0 = 1,
            .duration0 = 3,
            .level1 = 0,
            .duration1 = 9,
        },
        .bit1 = {
            .level0 = 1,
            .duration0 = 9,
            .level1 = 0,
            .duration1 = 3,
        },
        .flags.msb_first = 1,
    };

    if (rmt_new_tx_channel(&channel_config, &rgb_led_channel) != ESP_OK) {
        ESP_LOGW(TAG, "RGB LED init failed on GPIO%d", RGB_LED_GPIO);
        return;
    }

    if (rmt_new_bytes_encoder(&encoder_config, &rgb_led_encoder) != ESP_OK) {
        ESP_LOGW(TAG, "RGB LED encoder init failed");
        return;
    }

    if (rmt_enable(rgb_led_channel) != ESP_OK) {
        ESP_LOGW(TAG, "RGB LED channel enable failed");
        return;
    }

    rgb_led_ready = true;
}

static void rgb_led_set(uint8_t red, uint8_t green, uint8_t blue)
{
    uint8_t grb_data[3] = {green, red, blue};
    rmt_transmit_config_t transmit_config = {
        .loop_count = 0,
    };

    if (!rgb_led_ready) {
        return;
    }

    rmt_transmit(rgb_led_channel, rgb_led_encoder, grb_data, sizeof(grb_data), &transmit_config);
    rmt_tx_wait_all_done(rgb_led_channel, pdMS_TO_TICKS(100));
}

static void rgb_led_blink_green(int count)
{
    for (int i = 0; i < count; i++) {
        rgb_led_set(0, 48, 0);
        vTaskDelay(pdMS_TO_TICKS(150));
        rgb_led_set(0, 0, 0);
        vTaskDelay(pdMS_TO_TICKS(150));
    }
}






static esp_err_t lora_transfer(uint8_t address, const uint8_t *tx_data, uint8_t *rx_data, size_t length)
{
    uint8_t tx_buffer[LORA_MAX_PAYLOAD + 1] = {0};
    uint8_t rx_buffer[LORA_MAX_PAYLOAD + 1] = {0};
    spi_transaction_t transaction = {0};

    if (length > LORA_MAX_PAYLOAD) {
        return ESP_ERR_INVALID_SIZE;
    }

    tx_buffer[0] = address;
    if (tx_data != NULL && length > 0) {
        memcpy(&tx_buffer[1], tx_data, length);
    }

    transaction.length = (length + 1) * 8;
    transaction.tx_buffer = tx_buffer;
    transaction.rx_buffer = rx_buffer;

    esp_err_t result = spi_device_transmit(lora_spi, &transaction);
    if (result == ESP_OK && rx_data != NULL && length > 0) {
        memcpy(rx_data, &rx_buffer[1], length);
    }

    return result;
}

static uint8_t lora_read_reg(uint8_t address)
{
    uint8_t value = 0;
    lora_transfer(address & 0x7F, NULL, &value, 1);
    return value;
}

static void lora_write_reg(uint8_t address, uint8_t value)
{
    lora_transfer(address | 0x80, &value, NULL, 1);
}

static void lora_write_fifo(const uint8_t *data, size_t length)
{
    lora_transfer(REG_FIFO | 0x80, data, NULL, length);
}

static void lora_read_fifo(uint8_t *data, size_t length)
{
    lora_transfer(REG_FIFO & 0x7F, NULL, data, length);
}

static void lora_set_mode(uint8_t mode)
{
    lora_write_reg(REG_OP_MODE, MODE_LONG_RANGE_MODE | mode);
}

static bool lora_channel_clear(void)
{
    for (int attempt = 0; attempt < 3; attempt++) {
        lora_set_mode(MODE_STDBY);
        lora_write_reg(REG_DIO_MAPPING_1, 0x80);
        lora_write_reg(REG_IRQ_FLAGS, 0xFF);
        lora_write_reg(REG_IRQ_FLAGS_1, 0xFF);
        lora_set_mode(0x07); // CAD mode
        vTaskDelay(pdMS_TO_TICKS(10));

        uint8_t irq_flags_1 = lora_read_reg(REG_IRQ_FLAGS_1);
        lora_write_reg(REG_IRQ_FLAGS, 0xFF);
        lora_write_reg(REG_IRQ_FLAGS_1, 0xFF);
        lora_set_mode(MODE_STDBY);

        if ((irq_flags_1 & IRQ1_CAD_DONE_MASK) != 0 && (irq_flags_1 & IRQ1_CAD_DETECTED_MASK) == 0) {
            return true;
        }

        vTaskDelay(pdMS_TO_TICKS(20 + (attempt * 30)));
    }

    return false;
}

static void lora_set_frequency(uint32_t frequency_hz)
{
    uint64_t frf = ((uint64_t)frequency_hz << 19) / 32000000;
    lora_write_reg(REG_FRF_MSB, (uint8_t)(frf >> 16));
    lora_write_reg(REG_FRF_MID, (uint8_t)(frf >> 8));
    lora_write_reg(REG_FRF_LSB, (uint8_t)(frf >> 0));
}

static void lora_receive_mode(void)
{
    lora_write_reg(REG_DIO_MAPPING_1, 0x00);
    lora_write_reg(REG_IRQ_FLAGS, 0xFF);
    lora_set_mode(MODE_RX_CONTINUOUS);
}

static void IRAM_ATTR lora_dio0_isr_handler(void *arg)
{
    BaseType_t high_priority_task_woken = pdFALSE;
    SemaphoreHandle_t semaphore = radio_in_tx ? lora_tx_done_semaphore : lora_dio0_semaphore;

    if (semaphore != NULL) {
        xSemaphoreGiveFromISR(semaphore, &high_priority_task_woken);
    }

    if (high_priority_task_woken == pdTRUE) {
        portYIELD_FROM_ISR();
    }
}

static bool packet_seen(const char *source, uint32_t id)
{
    bool seen = false;
    TickType_t now = xTaskGetTickCount();
    TickType_t ttl_ticks = pdMS_TO_TICKS(SEEN_PACKET_TTL_MS);

    data_lock();
    for (size_t i = 0; i < seen_packet_count;) {
        if ((now - seen_packets[i].seen_tick) > ttl_ticks) {
            seen_packets[i] = seen_packets[seen_packet_count - 1];
            seen_packet_count--;
            continue;
        }

        if (seen_packets[i].id == id && strcmp(seen_packets[i].source, source) == 0) {
            seen = true;
            break;
        }

        i++;
    }

    data_unlock();
    return seen;
}

static void remember_packet(const char *source, uint32_t id)
{
    seen_packet_t *seen_packet;
    TickType_t now = xTaskGetTickCount();
    TickType_t ttl_ticks = pdMS_TO_TICKS(SEEN_PACKET_TTL_MS);
    size_t slot_index = 0;

    data_lock();
    for (size_t i = 0; i < seen_packet_count;) {
        if ((now - seen_packets[i].seen_tick) > ttl_ticks) {
            seen_packets[i] = seen_packets[seen_packet_count - 1];
            seen_packet_count--;
            continue;
        }

        i++;
    }

    if (seen_packet_count < MAX_SEEN_PACKETS) {
        seen_packet = &seen_packets[seen_packet_count++];
    } else {
        for (size_t i = 1; i < seen_packet_count; i++) {
            if ((now - seen_packets[i].seen_tick) > (now - seen_packets[slot_index].seen_tick)) {
                slot_index = i;
            }
        }
        seen_packet = &seen_packets[slot_index];
    }

    seen_packet->id = id;
    seen_packet->seen_tick = now;
    copy_field(seen_packet->source, sizeof(seen_packet->source), source);
    data_unlock();
}

static bool parse_mesh_packet(const char *packet, mesh_packet_t *parsed)
{
    char packet_copy[PACKET_LEN];
    char *fields[10] = {0};
    char *cursor = packet_copy;
    size_t field_count = 0;

    memset(parsed, 0, sizeof(*parsed));
    copy_field(packet_copy, sizeof(packet_copy), packet);

    while (field_count < sizeof(fields) / sizeof(fields[0]) && cursor != NULL) {
        fields[field_count++] = cursor;
        if (field_count == sizeof(fields) / sizeof(fields[0])) {
            break;
        }

        cursor = strchr(cursor, '|');
        if (cursor != NULL) {
            *cursor = '\0';
            cursor++;
        }
    }

    if (field_count < 10 || strcmp(fields[0], "BEMS") != 0) {
        return false;
    }

    parsed->valid = true;
    parsed->id = (uint32_t)strtoul(fields[1], NULL, 10);
    parsed->hops = strncmp(fields[6], "HOPS=", 5) == 0 ? atoi(fields[6] + 5) : 0;
    copy_field(parsed->source, sizeof(parsed->source), fields[2]);
    copy_field(parsed->destination, sizeof(parsed->destination), fields[3]);
    copy_field(parsed->type, sizeof(parsed->type), fields[4]);
    copy_field(parsed->priority, sizeof(parsed->priority), fields[5]);
    copy_field(parsed->relay, sizeof(parsed->relay), fields[7]);
    copy_field(parsed->location_raw, sizeof(parsed->location_raw), fields[8]);
    location_decode(fields[8], &parsed->location);
    copy_field(parsed->payload, sizeof(parsed->payload), fields[9]);
    return true;
}

static void build_forward_packet(const mesh_packet_t *parsed, char *packet, size_t packet_size)
{
    int next_hops = MAX(parsed->hops - 1, 0);
    char encoded_location[SITIO_LEN + BARANGAY_LEN + MUNICIPALITY_LEN + 2];

    location_encode(&parsed->location, encoded_location, sizeof(encoded_location));

    snprintf(packet, packet_size, "BEMS|%lu|%.*s|%.*s|%.*s|%.*s|HOPS=%d|%.*s|%s|%.*s",
             (unsigned long)parsed->id,
             31,
             parsed->source,
             31,
             parsed->destination,
             31,
             parsed->type,
             31,
             parsed->priority,
             next_hops,
             31,
             parsed->relay,
             encoded_location,
             48,
             parsed->payload);
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

static void send_ack_packet(const mesh_packet_t *parsed)
{
    char ack_packet[PACKET_LEN];
    char encoded_location[SITIO_LEN + BARANGAY_LEN + MUNICIPALITY_LEN + 2];

    location_encode(&node_config.location, encoded_location, sizeof(encoded_location));

    snprintf(ack_packet, sizeof(ack_packet), "BEMS|%lu|%.*s|%.*s|ACK|NORMAL|HOPS=0|RELAY=%u|LOC=%s|ACK for %lu",
             (unsigned long)parsed->id,
             31,
             node_id,
             31,
             parsed->source,
             1,
             encoded_location,
             (unsigned long)parsed->id);

    ESP_LOGI(TAG, "Sending ACK to %s for packet %lu", parsed->source, (unsigned long)parsed->id);
    lora_transmit(ack_packet);
}

static uint32_t sync_last_id_from_payload(const char *payload)
{
    if (strncmp(payload, "last_id=", 8) == 0) {
        return (uint32_t)strtoul(payload + 8, NULL, 10);
    }

    return 0;
}

static uint32_t ack_id_from_payload(const char *payload)
{
    if (strncmp(payload, "ACK for ", 8) == 0) {
        return (uint32_t)strtoul(payload + 8, NULL, 10);
    }

    return 0;
}

static bool is_control_packet_type(const char *type)
{
    return strcmp(type, "ACK") == 0 || strcmp(type, "SYNC_REQ") == 0 || strcmp(type, "SYNC_RESP") == 0 || strcmp(type, "TIME_SYNC") == 0;
}

static uint32_t current_epoch_seconds(void)
{
    return (uint32_t)(epoch_offset_sec + (int64_t)(xTaskGetTickCount() / configTICK_RATE_HZ));
}

static void apply_time_sync(uint32_t epoch, uint8_t distance)
{
    uint32_t now_ticks = xTaskGetTickCount();
    uint32_t now_seconds = now_ticks / configTICK_RATE_HZ;

    epoch_offset_sec = (int64_t)epoch - (int64_t)now_seconds;
    time_synced = true;
    time_sync_distance = distance;
    last_time_sync_broadcast_tick = 0;
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

static void send_time_sync_packet(uint32_t epoch, uint8_t distance, uint8_t hops)
{
    char packet[PACKET_LEN];
    char encoded_location[SITIO_LEN + BARANGAY_LEN + MUNICIPALITY_LEN + 2];
    uint32_t message_id;

    data_lock();
    message_id = ++packet_counter;
    save_packet_counter();
    data_unlock();

    location_encode(&node_config.location, encoded_location, sizeof(encoded_location));

    snprintf(packet, sizeof(packet), "BEMS|%lu|%.*s|ALL|TIME_SYNC|NORMAL|HOPS=%u|RELAY=1|LOC=%s|epoch=%lu~dist=%u",
             (unsigned long)message_id,
             31,
             node_id,
             hops,
             encoded_location,
             (unsigned long)epoch,
             distance);
    lora_transmit(packet);
}

static void broadcast_time_sync_if_synced(void)
{
    if (!time_synced) {
        return;
    }

    if ((xTaskGetTickCount() - last_time_sync_broadcast_tick) >= pdMS_TO_TICKS(15 * 60 * 1000)) {
        send_time_sync_packet(current_epoch_seconds(), time_sync_distance, 2);
        last_time_sync_broadcast_tick = xTaskGetTickCount();
    }
}

static bool is_private_destination_for_other_node(const char *destination, const char *requester)
{
    return strcmp(destination, "ALL") != 0 && strcmp(destination, requester) != 0;
}

static void send_sync_responses(const mesh_packet_t *request)
{
    emergency_message_t snapshot[MAX_MESSAGES];
    size_t snapshot_count = 0;
    size_t stored_message_count;
    uint32_t last_id = sync_last_id_from_payload(request->payload);
    char encoded_location[SITIO_LEN + BARANGAY_LEN + MUNICIPALITY_LEN + 2];

    location_encode(&node_config.location, encoded_location, sizeof(encoded_location));

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

        vTaskDelay(pdMS_TO_TICKS(200 + (esp_random() % 1001)));
        if (original_packet == NULL) {
            original_packet = snapshot[i].packet;
        }

        prefix_len = snprintf(sync_response, sizeof(sync_response), "BEMS|%lu|%.*s|%.*s|SYNC_RESP|NORMAL|HOPS=0|RELAY=0|LOC=%s|",
                              (unsigned long)snapshot[i].id,
                              31,
                              node_id,
                              31,
                              request->source,
                              encoded_location);
        original_len = strlen(original_packet);

        if (prefix_len < 0 || (size_t)prefix_len >= sizeof(sync_response) || (size_t)prefix_len + original_len > BEMS_MAX_PLAINTEXT) {
            ESP_LOGW(TAG, "Skipping oversized SYNC_RESP for packet %lu", (unsigned long)snapshot[i].id);
            continue;
        }
        copy_field(sync_response + prefix_len, sizeof(sync_response) - (size_t)prefix_len, original_packet);

        ESP_LOGI(TAG, "SYNC_RESP to %s for packet %lu", request->source, (unsigned long)snapshot[i].id);
        lora_transmit(sync_response);
    }
}

static void send_sync_request(uint32_t last_id)
{
    char sync_request[PACKET_LEN];
    char encoded_location[SITIO_LEN + BARANGAY_LEN + MUNICIPALITY_LEN + 2];
    uint32_t request_id;

    data_lock();
    request_id = ++packet_counter;
    save_packet_counter();
    data_unlock();

    location_encode(&node_config.location, encoded_location, sizeof(encoded_location));

    snprintf(sync_request, sizeof(sync_request), "BEMS|%lu|%.*s|ALL|SYNC_REQ|NORMAL|HOPS=1|RELAY=0|LOC=%s|last_id=%lu",
             (unsigned long)request_id,
             31,
             node_id,
             encoded_location,
             (unsigned long)last_id);

    ESP_LOGI(TAG, "Broadcasting SYNC_REQ with last_id=%lu", (unsigned long)last_id);
    lora_transmit(sync_request);
}

static void send_boot_sync_request(void)
{
    send_sync_request(highest_seen_id);
}

static void send_manual_sync_request(void)
{
    send_sync_request(0);
}

static void boot_sync_task(void *parameter)
{
    vTaskDelay(pdMS_TO_TICKS(1500));

    if (node_config.configured && lora_ready) {
        send_boot_sync_request();
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
    message->stored_epoch = time_synced ? current_epoch_seconds() : 0;
    snprintf(message->packet, sizeof(message->packet), "RSSI=%d SNR=%d | %.*s", rssi, snr, 250, packet);
    if (parsed->valid && !is_control_packet_type(parsed->type)) {
        update_highest_seen_id(message->id);
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

static bool lora_transmit(const char *packet)
{
    uint8_t frame[LORA_MAX_PAYLOAD];
    size_t length = 0;

    if (!lora_ready) {
        ESP_LOGW(TAG, "SX1278 is not ready; packet kept in local log only");
        return false;
    }

    if (!bems_encrypt_packet(packet, frame, sizeof(frame), &length)) {
        ESP_LOGW(TAG, "Failed to encrypt LoRa packet");
        return false;
    }

    if (lora_tx_done_semaphore == NULL) {
        ESP_LOGW(TAG, "LoRa TX done semaphore is not ready");
        return false;
    }

    if (!lora_channel_clear()) {
        ESP_LOGW(TAG, "Channel busy; TX skipped");
        return false;
    }

    xSemaphoreTake(lora_tx_done_semaphore, 0);
    lora_set_mode(MODE_STDBY);
    lora_write_reg(REG_DIO_MAPPING_1, 0x40);
    lora_write_reg(REG_IRQ_FLAGS, 0xFF);
    lora_write_reg(REG_FIFO_ADDR_PTR, 0x00);
    lora_write_fifo(frame, length);
    lora_write_reg(REG_PAYLOAD_LENGTH, length);
    radio_in_tx = true;
    lora_set_mode(MODE_TX);

    if (xSemaphoreTake(lora_tx_done_semaphore, pdMS_TO_TICKS(5000)) == pdTRUE) {
        radio_in_tx = false;
        lora_write_reg(REG_IRQ_FLAGS, 0xFF);
        lora_receive_mode();
        ESP_LOGI(TAG, "SX1278 encrypted TX done: %u bytes", (unsigned int)length);
        return true;
    }

    radio_in_tx = false;
    lora_write_reg(REG_IRQ_FLAGS, 0xFF);
    lora_receive_mode();
    ESP_LOGW(TAG, "SX1278 TX timeout");
    return false;
}

static void lora_rx_task(void *parameter)
{
    uint8_t payload[LORA_MAX_PAYLOAD + 1];
    char decrypted_packet[PACKET_LEN];

    while (true) {
        if (lora_dio0_semaphore != NULL) {
            xSemaphoreTake(lora_dio0_semaphore, portMAX_DELAY);
        }

        if (lora_ready && (lora_read_reg(REG_IRQ_FLAGS) & IRQ_RX_DONE_MASK) != 0) {
            uint8_t flags = lora_read_reg(REG_IRQ_FLAGS);
            lora_write_reg(REG_IRQ_FLAGS, 0xFF);

            if ((flags & IRQ_PAYLOAD_CRC_ERROR_MASK) == 0) {
                uint8_t length = lora_read_reg(REG_RX_NB_BYTES);
                uint8_t current_addr = lora_read_reg(REG_FIFO_RX_CURRENT_ADDR);
                int rssi = (int)lora_read_reg(REG_PKT_RSSI_VALUE) - 164;
                int snr = ((int8_t)lora_read_reg(REG_PKT_SNR_VALUE)) / 4;

                lora_write_reg(REG_FIFO_ADDR_PTR, current_addr);
                lora_read_fifo(payload, length);
                payload[length] = '\0';

                if (!bems_decrypt_frame(payload, length, decrypted_packet, sizeof(decrypted_packet))) {
                    ESP_LOGW(TAG, "Rejected unauthenticated LoRa frame RSSI=%d SNR=%d length=%u", rssi, snr, length);
                    continue;
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
                            send_sync_responses(&parsed);
                            continue;
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
                            continue;
                        }

                        if (strcmp(parsed.type, "TIME_SYNC") == 0) {
                            uint32_t epoch = time_sync_epoch_from_payload(parsed.payload);
                            uint8_t dist = time_sync_dist_from_payload(parsed.payload);

                            if ((epoch != 0) && (!time_synced || dist < time_sync_distance)) {
                                apply_time_sync(epoch, dist);
                            }
                            continue;
                        }

                        if (is_broadcast || is_for_me) {
                            store_received_packet(decrypted_packet, &parsed, rssi, snr);
                        }

                        if (is_for_me && !is_ack) {
                            send_ack_packet(&parsed);
                        }

                        if (is_ack && is_for_me) {
                            if (strcmp(parsed.source, node_id) != 0) {
                                duplicate_node_id_warning = true;
                            }
                            uint32_t ack_id = ack_id_from_payload(parsed.payload);
                            if (ack_id != 0) {
                                update_message_status(ack_id, node_id, "ACKED");
                            }
                        }

                        if (parsed.hops > 0 && !is_for_me && !is_ack && !is_sync_req && !is_sync_resp && strcmp(parsed.type, "TIME_SYNC") != 0) {
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
                    }
                } else {
                    mesh_packet_t raw_packet = {0};
                    store_received_packet(decrypted_packet, &raw_packet, rssi, snr);
                }
            }
        }
    }
}

static void lora_init(void)
{
    spi_bus_config_t bus_config = {
        .mosi_io_num = LORA_MOSI_GPIO,
        .miso_io_num = LORA_MISO_GPIO,
        .sclk_io_num = LORA_SCK_GPIO,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = LORA_MAX_PAYLOAD + 1,
    };
    spi_device_interface_config_t device_config = {
        .clock_speed_hz = 1000000,
        .mode = 0,
        .spics_io_num = LORA_NSS_GPIO,
        .queue_size = 1,
    };

    gpio_config_t reset_config = {
        .pin_bit_mask = 1ULL << LORA_RST_GPIO,
        .mode = GPIO_MODE_OUTPUT,
    };
    gpio_config_t dio0_config = {
        .pin_bit_mask = 1ULL << LORA_DIO0_GPIO,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_POSEDGE,
    };

    ESP_ERROR_CHECK(gpio_config(&reset_config));
    ESP_ERROR_CHECK(gpio_config(&dio0_config));

    lora_dio0_semaphore = xSemaphoreCreateBinary();
    if (lora_dio0_semaphore == NULL) {
        ESP_LOGE(TAG, "Failed to create LoRa DIO0 semaphore");
        return;
    }

    lora_tx_done_semaphore = xSemaphoreCreateBinary();
    if (lora_tx_done_semaphore == NULL) {
        ESP_LOGE(TAG, "Failed to create LoRa TX done semaphore");
        return;
    }

    esp_err_t isr_result = gpio_install_isr_service(0);
    if (isr_result != ESP_OK && isr_result != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "Failed to install GPIO ISR service: %s", esp_err_to_name(isr_result));
        return;
    }

    ESP_ERROR_CHECK(gpio_isr_handler_add(LORA_DIO0_GPIO, lora_dio0_isr_handler, NULL));

    gpio_set_level(LORA_RST_GPIO, 0);
    vTaskDelay(pdMS_TO_TICKS(10));
    gpio_set_level(LORA_RST_GPIO, 1);
    vTaskDelay(pdMS_TO_TICKS(20));

    ESP_ERROR_CHECK(spi_bus_initialize(LORA_SPI_HOST, &bus_config, SPI_DMA_CH_AUTO));
    ESP_ERROR_CHECK(spi_bus_add_device(LORA_SPI_HOST, &device_config, &lora_spi));

    uint8_t version = lora_read_reg(REG_VERSION);
    if (version != 0x12) {
        ESP_LOGE(TAG, "SX1278 not detected. REG_VERSION=0x%02X, check wiring and 3.3V power", version);
        return;
    }

    lora_set_mode(MODE_SLEEP);
    vTaskDelay(pdMS_TO_TICKS(10));
    lora_set_frequency(LORA_FREQUENCY_HZ);
    lora_write_reg(REG_FIFO_TX_BASE_ADDR, 0x00);
    lora_write_reg(REG_FIFO_RX_BASE_ADDR, 0x00);
    lora_write_reg(REG_LNA, lora_read_reg(REG_LNA) | 0x03);
    lora_write_reg(REG_MODEM_CONFIG_1, LORA_MODEM_CONFIG_1);
    lora_write_reg(REG_MODEM_CONFIG_2, LORA_MODEM_CONFIG_2);
    lora_write_reg(REG_MODEM_CONFIG_3, LORA_MODEM_CONFIG_3);
    lora_write_reg(REG_PREAMBLE_MSB, 0x00);
    lora_write_reg(REG_PREAMBLE_LSB, 0x08);
    lora_write_reg(REG_SYNC_WORD, 0x12);
    lora_write_reg(REG_PA_CONFIG, 0x8F);

    lora_ready = true;
    lora_receive_mode();
    xTaskCreate(lora_rx_task, "lora_rx_task", 4096, NULL, 6, NULL);

    ESP_LOGI(TAG, "SX1278 ready on 433 MHz");
}

static void update_message_status(uint32_t id, const char *source, const char *status)
{
    data_lock();
    message_store_update_status(id, source, status);
    data_unlock();
}

static void retry_tracker_add_by_value(uint32_t id, const char *source, const char *destination, const char *priority)
{
    if (strcmp(priority, "HIGH") != 0 || strcmp(destination, "ALL") == 0) {
        return;
    }

    data_lock();
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
    data_unlock();
}

static void retry_tracker_task(void *parameter)
{
    while (true) {
        TickType_t now = xTaskGetTickCount();
        data_lock();
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
        data_unlock();
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

static void time_sync_task(void *parameter)
{
    while (true) {
        broadcast_time_sync_if_synced();
        vTaskDelay(pdMS_TO_TICKS(30000));
    }
}

static void copy_node_id(char *destination, size_t destination_size, const char *source)
{
    size_t write_index = 0;

    if (destination_size == 0) {
        return;
    }

    while (*source != '\0' && write_index < destination_size - 1) {
        unsigned char character = (unsigned char)*source++;
        if (isalnum(character) || character == '-' || character == '_') {
            destination[write_index++] = (char)toupper(character);
        }
    }

    destination[write_index] = '\0';
}

static void nvs_get_string_or_default(nvs_handle_t handle, const char *key, char *value, size_t value_size, const char *fallback)
{
    esp_err_t result = nvs_get_str(handle, key, value, &value_size);

    if (result != ESP_OK || value[0] == '\0') {
        copy_field(value, value_size, fallback);
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

static void load_highest_seen_id(void)
{
    nvs_handle_t handle;
    uint32_t stored_id = 0;

    if (nvs_open(CONFIG_NAMESPACE, NVS_READONLY, &handle) != ESP_OK) {
        ESP_LOGI(TAG, "No saved highest seen ID; starting at 0");
        return;
    }

    if (nvs_get_u32(handle, HIGHEST_SEEN_ID_KEY, &stored_id) == ESP_OK) {
        highest_seen_id = stored_id;
        ESP_LOGI(TAG, "Highest seen ID restored: %lu", (unsigned long)highest_seen_id);
    } else {
        ESP_LOGI(TAG, "No saved highest seen ID; starting at 0");
    }

    nvs_close(handle);
}

static void save_highest_seen_id(void)
{
    nvs_handle_t handle;
    esp_err_t result = nvs_open(CONFIG_NAMESPACE, NVS_READWRITE, &handle);

    if (result != ESP_OK) {
        ESP_LOGW(TAG, "Failed to open NVS for highest seen ID: %s", esp_err_to_name(result));
        return;
    }

    result = nvs_set_u32(handle, HIGHEST_SEEN_ID_KEY, highest_seen_id);
    if (result == ESP_OK) {
        result = nvs_commit(handle);
    }

    if (result != ESP_OK) {
        ESP_LOGW(TAG, "Failed to save highest seen ID: %s", esp_err_to_name(result));
    }

    nvs_close(handle);
}

static void update_highest_seen_id(uint32_t id)
{
    if (id > highest_seen_id) {
        highest_seen_id = id;
        save_highest_seen_id();
    }
}

static void config_set_defaults(void)
{
    node_config.configured = false;
    copy_field(node_config.node_id, sizeof(node_config.node_id), node_id);
    copy_field(node_config.node_name, sizeof(node_config.node_name), "Unconfigured Node");
    copy_field(node_config.node_role, sizeof(node_config.node_role), "relay-only");
    copy_field(node_config.location.sitio, sizeof(node_config.location.sitio), "");
    copy_field(node_config.location.barangay, sizeof(node_config.location.barangay), "Unknown");
    copy_field(node_config.location.municipality, sizeof(node_config.location.municipality), "");
    copy_field(node_config.default_destination, sizeof(node_config.default_destination), "BRGY001");
    copy_field(node_config.default_priority, sizeof(node_config.default_priority), "NORMAL");
    copy_field(node_config.ap_password, sizeof(node_config.ap_password), AP_PASSWORD);
    copy_field(node_config.web_pin, sizeof(node_config.web_pin), DEFAULT_WEB_PIN);
    copy_field(node_config.duress_pin, sizeof(node_config.duress_pin), "");
    copy_field(node_config.network_key, sizeof(node_config.network_key), DEFAULT_NETWORK_KEY);
}

static void apply_config_identity(void)
{
    if (node_config.configured) {
        copy_field(node_id, sizeof(node_id), node_config.node_id);
        snprintf(ap_ssid, sizeof(ap_ssid), "BMesh-%.*s", 24, node_config.node_id);
    } else {
        snprintf(ap_ssid, sizeof(ap_ssid), "BMesh-SETUP-%.*s", 18, node_id + 4);
    }
}

static void load_node_config(void)
{
    nvs_handle_t handle;
    uint8_t configured = 0;
    bool migrated_location = false;
    char legacy_location[FIELD_LEN] = {0};

    config_set_defaults();

    if (nvs_open(CONFIG_NAMESPACE, NVS_READWRITE, &handle) != ESP_OK) {
        apply_config_identity();
        return;
    }

    nvs_get_u8(handle, "configured", &configured);
    nvs_get_string_or_default(handle, "node_id", node_config.node_id, sizeof(node_config.node_id), node_id);
    nvs_get_string_or_default(handle, "node_name", node_config.node_name, sizeof(node_config.node_name), "Mesh Node");
    nvs_get_string_or_default(handle, "node_role", node_config.node_role, sizeof(node_config.node_role), "relay-only");
    nvs_get_string_or_default(handle, "default_dest", node_config.default_destination, sizeof(node_config.default_destination), "BRGY001");
    nvs_get_string_or_default(handle, "default_priority", node_config.default_priority, sizeof(node_config.default_priority), "NORMAL");
    nvs_get_string_or_default(handle, "ap_password", node_config.ap_password, sizeof(node_config.ap_password), AP_PASSWORD);
    nvs_get_string_or_default(handle, "web_pin", node_config.web_pin, sizeof(node_config.web_pin), DEFAULT_WEB_PIN);
    nvs_get_string_or_default(handle, "duress_pin", node_config.duress_pin, sizeof(node_config.duress_pin), "");
    nvs_get_string_or_default(handle, "network_key", node_config.network_key, sizeof(node_config.network_key), DEFAULT_NETWORK_KEY);

    if (nvs_get_str(handle, "sitio", node_config.location.sitio, &(size_t){sizeof(node_config.location.sitio)}) != ESP_OK) {
        copy_field(node_config.location.sitio, sizeof(node_config.location.sitio), "");
    }
    if (nvs_get_str(handle, "barangay", node_config.location.barangay, &(size_t){sizeof(node_config.location.barangay)}) != ESP_OK) {
        if (nvs_get_str(handle, "location", legacy_location, &(size_t){sizeof(legacy_location)}) == ESP_OK && legacy_location[0] != '\0') {
            location_decode(legacy_location, &node_config.location);
            migrated_location = true;
        } else {
            copy_field(node_config.location.barangay, sizeof(node_config.location.barangay), "Unknown");
        }
    }
    if (nvs_get_str(handle, "municipality", node_config.location.municipality, &(size_t){sizeof(node_config.location.municipality)}) != ESP_OK) {
        copy_field(node_config.location.municipality, sizeof(node_config.location.municipality), "");
    }

    nvs_close(handle);

    node_config.configured = configured == 1;
    apply_config_identity();

    if (migrated_location) {
        ESP_LOGI(TAG, "Migrating legacy location key to structured fields");
        save_node_config(&node_config);
    }

    char encoded_location[SITIO_LEN + BARANGAY_LEN + MUNICIPALITY_LEN + 2];
    location_encode(&node_config.location, encoded_location, sizeof(encoded_location));

    ESP_LOGI(TAG, "Config loaded: configured=%d node=%s name=%s location=%s",
             node_config.configured,
             node_config.node_id,
             node_config.node_name,
             encoded_location);
}

static esp_err_t save_node_config(const node_config_t *config)
{
    nvs_handle_t handle;
    esp_err_t result = nvs_open(CONFIG_NAMESPACE, NVS_READWRITE, &handle);

    if (result != ESP_OK) {
        return result;
    }

    result = nvs_set_u8(handle, "configured", config->configured ? 1 : 0);
    if (result == ESP_OK) {
        result = nvs_set_str(handle, "node_id", config->node_id);
    }
    if (result == ESP_OK) {
        result = nvs_set_str(handle, "node_name", config->node_name);
    }
    if (result == ESP_OK) {
        result = nvs_set_str(handle, "node_role", config->node_role);
    }
    if (result == ESP_OK) {
        result = nvs_set_str(handle, "sitio", config->location.sitio);
    }
    if (result == ESP_OK) {
        result = nvs_set_str(handle, "barangay", config->location.barangay);
    }
    if (result == ESP_OK) {
        result = nvs_set_str(handle, "municipality", config->location.municipality);
    }
    if (result == ESP_OK) {
        result = nvs_set_str(handle, "default_dest", config->default_destination);
    }
    if (result == ESP_OK) {
        result = nvs_set_str(handle, "default_priority", config->default_priority);
    }
    if (result == ESP_OK) {
        result = nvs_set_str(handle, "ap_password", config->ap_password);
    }
    if (result == ESP_OK) {
        result = nvs_set_str(handle, "web_pin", config->web_pin);
    }
    if (result == ESP_OK) {
        result = nvs_set_str(handle, "duress_pin", config->duress_pin);
    }
    if (result == ESP_OK) {
        result = nvs_set_str(handle, "network_key", config->network_key);
    }
    if (result == ESP_OK) {
        result = nvs_erase_key(handle, "location");
    }
    if (result == ESP_OK) {
        result = nvs_commit(handle);
    }

    nvs_close(handle);
    return result;
}

static void erase_node_config(void)
{
    nvs_handle_t handle;

    if (nvs_open(CONFIG_NAMESPACE, NVS_READWRITE, &handle) == ESP_OK) {
        nvs_erase_all(handle);
        nvs_commit(handle);
        nvs_close(handle);
    }
}

static void init_factory_reset_button(void)
{
    gpio_config_t boot_button_config = {
        .pin_bit_mask = 1ULL << BOOT_BUTTON_GPIO,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
    };

    ESP_ERROR_CHECK(gpio_config(&boot_button_config));
}

static void factory_reset_button_task(void *parameter)
{
    bool warning_blinked = false;
    int held_ms = 0;

    while (true) {
        if (gpio_get_level(BOOT_BUTTON_GPIO) == 0) {
            held_ms += 100;

            if (!warning_blinked && held_ms >= RESET_WARNING_MS) {
                ESP_LOGW(TAG, "BOOT held for 5 seconds. Keep holding for factory reset.");
                rgb_led_blink_green(1);
                warning_blinked = true;
            }

            if (held_ms >= FACTORY_RESET_HOLD_MS) {
                ESP_LOGW(TAG, "BOOT held for 10 seconds. Factory reset confirmed.");
                rgb_led_blink_green(3);
                erase_node_config();
                vTaskDelay(pdMS_TO_TICKS(500));
                esp_restart();
            }
        } else {
            if (held_ms > 0 && held_ms < FACTORY_RESET_HOLD_MS) {
                ESP_LOGI(TAG, "Factory reset hold cancelled");
            }
            held_ms = 0;
            warning_blinked = false;
        }

        vTaskDelay(pdMS_TO_TICKS(100));
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

    location_encode(&node_config.location, encoded_location, sizeof(encoded_location));

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
    message->stored_epoch = time_synced ? current_epoch_seconds() : 0;
    build_packet(message);
    compute_thread_key(message->thread_key, sizeof(message->thread_key), message->source, message->destination);
    message->origin_location = node_config.location;
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
        update_message_status(queued_id, queued_source, "SENT");
        retry_tracker_add_by_value(queued_id, queued_source, queued_destination, queued_priority);
    } else {
        update_message_status(queued_id, queued_source, "FAILED");
    }
}

static void init_littlefs(void)
{
    esp_vfs_littlefs_conf_t config = {
        .base_path = LITTLEFS_BASE_PATH,
        .partition_label = "storage",
        .format_if_mount_failed = true,
        .dont_mount = false,
    };
    size_t total_bytes = 0;
    size_t used_bytes = 0;
    esp_err_t result = esp_vfs_littlefs_register(&config);

    if (result != ESP_OK) {
        ESP_LOGE(TAG, "LittleFS mount failed for partition '%s': %s", config.partition_label, esp_err_to_name(result));
        return;
    }
    littlefs_mounted = true;
    result = esp_littlefs_info(config.partition_label, &total_bytes, &used_bytes);
    if (result == ESP_OK) {
        ESP_LOGI(TAG, "LittleFS mounted at %s: %u/%u bytes used", config.base_path, (unsigned int)used_bytes, (unsigned int)total_bytes);
    } else {
        ESP_LOGW(TAG, "LittleFS mounted, but space query failed: %s", esp_err_to_name(result));
    }
}

static esp_err_t setup_handler(httpd_req_t *request)
{
    char body[384] = {0};
    char raw_node_id[FIELD_LEN] = {0};
    node_config_t new_config = {0};
    int received = 0;

    while (received < request->content_len && received < (int)sizeof(body) - 1) {
        int ret = httpd_req_recv(request, body + received, MIN(request->content_len - received, (int)sizeof(body) - 1 - received));
        if (ret <= 0) {
            return ESP_FAIL;
        }
        received += ret;
    }

    form_value(body, "node_id", raw_node_id, sizeof(raw_node_id));
    copy_node_id(new_config.node_id, sizeof(new_config.node_id), raw_node_id);
    form_value(body, "node_name", new_config.node_name, sizeof(new_config.node_name));
    form_value(body, "node_role", new_config.node_role, sizeof(new_config.node_role));
    form_value(body, "sitio", new_config.location.sitio, sizeof(new_config.location.sitio));
    copy_field_no_delims(new_config.location.sitio, sizeof(new_config.location.sitio), new_config.location.sitio);
    form_value(body, "barangay", new_config.location.barangay, sizeof(new_config.location.barangay));
    copy_field_no_delims(new_config.location.barangay, sizeof(new_config.location.barangay), new_config.location.barangay);
    form_value(body, "municipality", new_config.location.municipality, sizeof(new_config.location.municipality));
    copy_field_no_delims(new_config.location.municipality, sizeof(new_config.location.municipality), new_config.location.municipality);
    form_value(body, "default_destination", new_config.default_destination, sizeof(new_config.default_destination));
    form_value(body, "default_priority", new_config.default_priority, sizeof(new_config.default_priority));
    form_value(body, "ap_password", new_config.ap_password, sizeof(new_config.ap_password));
    form_value(body, "web_pin", new_config.web_pin, sizeof(new_config.web_pin));
    form_value(body, "duress_pin", new_config.duress_pin, sizeof(new_config.duress_pin));
    form_value(body, "network_key", new_config.network_key, sizeof(new_config.network_key));
    new_config.configured = true;
    if (new_config.node_id[0] == '\0') {
        copy_field(new_config.node_id, sizeof(new_config.node_id), node_id);
    }
    if (new_config.node_name[0] == '\0') {
        copy_field(new_config.node_name, sizeof(new_config.node_name), "Mesh Node");
    }
    if (new_config.location.barangay[0] == '\0') {
        copy_field(new_config.location.barangay, sizeof(new_config.location.barangay), "Unknown");
    }
    if (new_config.default_destination[0] == '\0') {
        copy_field(new_config.default_destination, sizeof(new_config.default_destination), "BRGY001");
    }
    if (new_config.node_role[0] == '\0') {
        copy_field(new_config.node_role, sizeof(new_config.node_role), "relay-only");
    }
    if (new_config.default_priority[0] == '\0') {
        copy_field(new_config.default_priority, sizeof(new_config.default_priority), "NORMAL");
    }
    if (new_config.ap_password[0] == '\0') {
        copy_field(new_config.ap_password, sizeof(new_config.ap_password), AP_PASSWORD);
    }
    if (new_config.web_pin[0] == '\0') {
        copy_field(new_config.web_pin, sizeof(new_config.web_pin), DEFAULT_WEB_PIN);
    }
    if (new_config.duress_pin[0] == '\0') {
        copy_field(new_config.duress_pin, sizeof(new_config.duress_pin), "");
    }
    if (new_config.network_key[0] == '\0') {
        copy_field(new_config.network_key, sizeof(new_config.network_key), DEFAULT_NETWORK_KEY);
    }

    ESP_ERROR_CHECK(save_node_config(&new_config));
    http_portal_send_file(request, "reboot.html");
    vTaskDelay(pdMS_TO_TICKS(500));
    esp_restart();
    return ESP_OK;
}

static esp_err_t reset_handler(httpd_req_t *request)
{
    esp_err_t session_result = http_auth_require_session(request);
    if (session_result != ESP_OK) {
        return session_result;
    }

    erase_node_config();
    http_portal_send_file(request, "reboot.html");
    vTaskDelay(pdMS_TO_TICKS(500));
    esp_restart();
    return ESP_OK;
}

static esp_err_t send_handler(httpd_req_t *request)
{
    char body[384] = {0};
    char destination[FIELD_LEN];
    char sender_name[FIELD_LEN] = {0};
    char type[FIELD_LEN] = "TEST";
    char priority[FIELD_LEN] = "NORMAL";
    char payload[PAYLOAD_LEN] = "No message";
    int received = 0;
    esp_err_t session_result = http_auth_require_session(request);

    if (session_result != ESP_OK) {
        return session_result;
    }

    while (received < request->content_len && received < (int)sizeof(body) - 1) {
        int ret = httpd_req_recv(request, body + received, MIN(request->content_len - received, (int)sizeof(body) - 1 - received));
        if (ret <= 0) {
            return ESP_FAIL;
        }
        received += ret;
    }

    copy_field(destination, sizeof(destination), node_config.default_destination);
    form_value(body, "sender_name", sender_name, sizeof(sender_name));
    form_value(body, "destination", destination, sizeof(destination));
    copy_field_no_delims(destination, sizeof(destination), destination);
    form_value(body, "type", type, sizeof(type));
    copy_field_no_delims(type, sizeof(type), type);
    form_value(body, "priority", priority, sizeof(priority));
    copy_field_no_delims(priority, sizeof(priority), priority);
    form_value(body, "payload", payload, sizeof(payload));

    if ((xTaskGetTickCount() - last_send_tick) < pdMS_TO_TICKS(10000)) {
        httpd_resp_set_status(request, "429 Too Many Requests");
        httpd_resp_set_type(request, "text/html");
        return httpd_resp_send(request, "<!doctype html><html><body><h2>Slow down.</h2><p>Please wait before sending again.</p></body></html>", HTTPD_RESP_USE_STRLEN);
    }

    last_send_tick = xTaskGetTickCount();
    if (sender_name[0] != '\0') {
        char named_payload[PAYLOAD_LEN];
        size_t prefix_len;

        named_payload[0] = '\0';
        copy_field(named_payload, sizeof(named_payload), sender_name);
        prefix_len = strlen(named_payload);
        if (prefix_len < sizeof(named_payload) - 3) {
            named_payload[prefix_len++] = ':';
            named_payload[prefix_len++] = ' ';
            named_payload[prefix_len] = '\0';
            copy_field(named_payload + prefix_len, sizeof(named_payload) - prefix_len, payload);
        }
        copy_field(payload, sizeof(payload), named_payload);
    }

    queue_message(destination, type, priority, payload);
    return http_auth_send_redirect(request, "/");
}

static void start_http_server(void)
{
    static http_messages_context_t messages_context = {
        .require_session = http_auth_require_session,
    };
    static http_status_context_t status_context;
    static http_time_context_t time_context;
    static http_sync_context_t sync_context;
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = HTTP_PORT;
    config.max_uri_handlers = 16;
    config.uri_match_fn = httpd_uri_match_wildcard;
    config.stack_size = 20480;

    status_context = (http_status_context_t){
        .require_session = http_auth_require_session,
        .current_epoch_seconds = current_epoch_seconds,
        .node_id = node_id,
        .node_name = node_config.node_name,
        .node_role = node_config.node_role,
        .location = node_config.location.barangay,
        .ssid = ap_ssid,
        .configured = &node_config.configured,
        .duplicate_node_id_warning = &duplicate_node_id_warning,
        .time_synced = &time_synced,
    };
    time_context = (http_time_context_t){
        .apply_time_sync = apply_time_sync,
        .send_time_sync_packet = send_time_sync_packet,
    };
    sync_context = (http_sync_context_t){
        .send_manual_sync_request = send_manual_sync_request,
    };

    const httpd_uri_t routes[] = {
        {.uri = "/", .method = HTTP_GET, .handler = http_portal_index_handler},
        {.uri = "/login", .method = HTTP_POST, .handler = http_auth_login_handler},
        {.uri = "/setup", .method = HTTP_POST, .handler = setup_handler},
        {.uri = "/reset", .method = HTTP_POST, .handler = reset_handler},
        {.uri = "/settime", .method = HTTP_POST, .handler = http_time_handler, .user_ctx = &time_context},
        {.uri = "/send", .method = HTTP_POST, .handler = send_handler},
        {.uri = "/sync", .method = HTTP_POST, .handler = http_sync_handler, .user_ctx = &sync_context},
        {.uri = "/api/status", .method = HTTP_GET, .handler = http_status_handler, .user_ctx = &status_context},
        {.uri = "/api/messages", .method = HTTP_GET, .handler = http_messages_handler, .user_ctx = &messages_context},
        {.uri = "/generate_204", .method = HTTP_GET, .handler = http_portal_captive_handler},
        {.uri = "/gen_204", .method = HTTP_GET, .handler = http_portal_captive_handler},
        {.uri = "/hotspot-detect.html", .method = HTTP_GET, .handler = http_portal_captive_handler},
        {.uri = "/connecttest.txt", .method = HTTP_GET, .handler = http_portal_captive_handler},
        {.uri = "/ncsi.txt", .method = HTTP_GET, .handler = http_portal_captive_handler},
        {.uri = "/*", .method = HTTP_GET, .handler = http_portal_captive_handler},
    };

    ESP_ERROR_CHECK(httpd_start(&http_server, &config));

    for (size_t i = 0; i < sizeof(routes) / sizeof(routes[0]); i++) {
        ESP_ERROR_CHECK(httpd_register_uri_handler(http_server, &routes[i]));
    }
}

static void dns_task(void *parameter)
{
    const uint32_t ap_ip = inet_addr("192.168.4.1");
    uint8_t buffer[512];
    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_IP);
    struct sockaddr_in server_addr = {
        .sin_family = AF_INET,
        .sin_port = htons(DNS_PORT),
        .sin_addr.s_addr = htonl(INADDR_ANY),
    };

    if (sock < 0 || bind(sock, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0) {
        ESP_LOGE(TAG, "DNS server failed");
        vTaskDelete(NULL);
        return;
    }

    while (true) {
        struct sockaddr_in client_addr;
        socklen_t client_len = sizeof(client_addr);
        int len = recvfrom(sock, buffer, sizeof(buffer), 0, (struct sockaddr *)&client_addr, &client_len);

        if (len < 12) {
            continue;
        }

        int question_end = 12;
        while (question_end < len && buffer[question_end] != 0) {
            question_end += buffer[question_end] + 1;
        }

        if (question_end + 5 > len) {
            continue;
        }

        buffer[2] = 0x81;
        buffer[3] = 0x80;
        buffer[6] = 0x00;
        buffer[7] = 0x01;
        buffer[8] = 0x00;
        buffer[9] = 0x00;
        buffer[10] = 0x00;
        buffer[11] = 0x00;

        int answer = question_end + 5;
        if (answer + 16 > (int)sizeof(buffer)) {
            continue;
        }

        buffer[answer++] = 0xC0;
        buffer[answer++] = 0x0C;
        buffer[answer++] = 0x00;
        buffer[answer++] = 0x01;
        buffer[answer++] = 0x00;
        buffer[answer++] = 0x01;
        buffer[answer++] = 0x00;
        buffer[answer++] = 0x00;
        buffer[answer++] = 0x00;
        buffer[answer++] = 0x3C;
        buffer[answer++] = 0x00;
        buffer[answer++] = 0x04;
        memcpy(&buffer[answer], &ap_ip, 4);
        answer += 4;

        sendto(sock, buffer, answer, 0, (struct sockaddr *)&client_addr, client_len);
    }
}

static void init_identity(void)
{
    uint8_t mac[6];

    ESP_ERROR_CHECK(esp_read_mac(mac, ESP_MAC_WIFI_SOFTAP));
    snprintf(node_id, sizeof(node_id), "NODE%02X%02X", mac[4], mac[5]);
    snprintf(ap_ssid, sizeof(ap_ssid), "BarangayMesh-SETUP-%02X%02X", mac[4], mac[5]);
    packet_counter = esp_random() & 0xFFFF;
}

static void start_wifi_ap(void)
{
    wifi_init_config_t wifi_init_config = WIFI_INIT_CONFIG_DEFAULT();
    wifi_config_t wifi_config = {0};

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_ap();
    ESP_ERROR_CHECK(esp_wifi_init(&wifi_init_config));

    copy_field((char *)wifi_config.ap.ssid, sizeof(wifi_config.ap.ssid), ap_ssid);
    wifi_config.ap.ssid_len = strlen(ap_ssid);
    wifi_config.ap.channel = AP_CHANNEL;
    wifi_config.ap.max_connection = AP_MAX_CONNECTIONS;
    wifi_config.ap.authmode = WIFI_AUTH_OPEN;

    if (strlen(node_config.ap_password) >= 8) {
        copy_field((char *)wifi_config.ap.password, sizeof(wifi_config.ap.password), node_config.ap_password);
        wifi_config.ap.authmode = WIFI_AUTH_WPA2_PSK;
    }

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "Captive portal AP started: %s", ap_ssid);
    ESP_LOGI(TAG, "Open http://192.168.4.1 after connecting");
}

void app_main(void)
{
    esp_err_t nvs_status = nvs_flash_init();
    if (nvs_status == ESP_ERR_NVS_NO_FREE_PAGES || nvs_status == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ESP_ERROR_CHECK(nvs_flash_init());
    } else {
        ESP_ERROR_CHECK(nvs_status);
    }

    data_mutex = xSemaphoreCreateMutex();
    ESP_ERROR_CHECK(data_mutex == NULL ? ESP_ERR_NO_MEM : ESP_OK);

    init_identity();
    http_portal_init(&(http_portal_context_t){
        .configured = &node_config.configured,
        .littlefs_mounted = &littlefs_mounted,
        .littlefs_base_path = LITTLEFS_BASE_PATH,
    });
    http_auth_init(&(http_auth_context_t){
        .configured = &node_config.configured,
        .web_pin = node_config.web_pin,
        .send_portal_file = http_portal_send_file,
    });
    load_packet_counter();
    load_highest_seen_id();
    rgb_led_init();
    init_factory_reset_button();
    load_node_config();
    message_store_load_messages_from_nvs(node_id);
    lora_init();
    init_littlefs();
    xTaskCreate(boot_sync_task, "boot_sync_task", 4096, NULL, 4, NULL);
    xTaskCreate(retry_tracker_task, "retry_tracker_task", 4096, NULL, 3, NULL);
    xTaskCreate(time_sync_task, "time_sync_task", 3072, NULL, 2, NULL);
    start_wifi_ap();
    start_http_server();
    xTaskCreate(factory_reset_button_task, "factory_reset_button_task", 3072, NULL, 7, NULL);
    xTaskCreate(dns_task, "dns_task", 4096, NULL, 5, NULL);

    ESP_LOGI(TAG, "Universal mesh node portal ready as %s", node_id);
}



