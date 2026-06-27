#include <ctype.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/param.h>

#include "driver/gpio.h"
#include "driver/rmt_tx.h"
#include "driver/spi_master.h"
#include "esp_event.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_random.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/inet.h"
#include "lwip/sockets.h"
#include "nvs.h"
#include "nvs_flash.h"

#define AP_CHANNEL 6
#define AP_MAX_CONNECTIONS 4
#define DNS_PORT 53
#define HTTP_PORT 80
#define MAX_MESSAGES 16
#define FIELD_LEN 32
#define PAYLOAD_LEN 160
#define PACKET_LEN 320
#define BOOT_BUTTON_GPIO 0
#define RGB_LED_GPIO 48
#define FACTORY_RESET_HOLD_MS 10000
#define RESET_WARNING_MS 5000
#define CONFIG_NAMESPACE "bems_config"

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
#define REG_VERSION 0x42

#define MODE_LONG_RANGE_MODE 0x80
#define MODE_SLEEP 0x00
#define MODE_STDBY 0x01
#define MODE_TX 0x03
#define MODE_RX_CONTINUOUS 0x05

#define IRQ_TX_DONE_MASK 0x08
#define IRQ_PAYLOAD_CRC_ERROR_MASK 0x20
#define IRQ_RX_DONE_MASK 0x40

typedef struct {
    uint32_t id;
    char direction[8];
    char source[FIELD_LEN];
    char destination[FIELD_LEN];
    char type[FIELD_LEN];
    char priority[FIELD_LEN];
    char payload[PAYLOAD_LEN];
    char packet[PACKET_LEN];
} emergency_message_t;

typedef struct {
    bool configured;
    bool relay_enabled;
    char node_id[FIELD_LEN];
    char node_name[FIELD_LEN];
    char location[FIELD_LEN];
    char default_destination[FIELD_LEN];
} node_config_t;

typedef struct {
    uint32_t id;
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
    char location[FIELD_LEN];
    char payload[PAYLOAD_LEN];
} mesh_packet_t;

static const char *TAG = "barangay_mesh";
static const char *AP_PASSWORD = "";

static char node_id[FIELD_LEN];
static char ap_ssid[FIELD_LEN];
static node_config_t node_config;
static emergency_message_t messages[MAX_MESSAGES];
static seen_packet_t seen_packets[MAX_MESSAGES];
static size_t message_count;
static size_t seen_packet_count;
static uint32_t packet_counter;
static httpd_handle_t http_server;
static spi_device_handle_t lora_spi;
static rmt_channel_handle_t rgb_led_channel;
static rmt_encoder_handle_t rgb_led_encoder;
static bool rgb_led_ready;
static bool lora_ready;

static void copy_field(char *destination, size_t destination_size, const char *source);

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

static const char INDEX_HTML[] =
    "<!doctype html><html><head><meta name=viewport content='width=device-width,initial-scale=1'>"
    "<title>Barangay Mesh</title><style>"
    ":root{font-family:Arial,sans-serif;color:#17202a;background:#f5f7f9}"
    "body{margin:0}.top{background:#b91c1c;color:white;padding:16px}.wrap{max-width:760px;margin:auto;padding:16px}"
    ".card{background:white;border:1px solid #d8dee4;border-radius:8px;padding:16px;margin:12px 0}"
    "label{display:block;font-weight:700;margin-top:12px}input,select,textarea,button{box-sizing:border-box;width:100%;font:inherit;padding:12px;margin-top:6px;border-radius:6px;border:1px solid #b8c0cc}"
    "textarea{min-height:94px}button{background:#b91c1c;color:white;border:0;font-weight:700;margin-top:16px}"
    ".row{display:grid;grid-template-columns:1fr 1fr;gap:10px}.muted{color:#5f6b7a;font-size:14px}.msg{border-top:1px solid #e5e7eb;padding:10px 0;word-break:break-word}"
    "@media(max-width:620px){.row{grid-template-columns:1fr}}"
    "</style></head><body><div class=top><div class=wrap><h2>Barangay Emergency Mesh</h2>"
    "<div id=status>Loading status...</div></div></div><main class=wrap>"
    "<section class=card><h3>Send Message</h3><form method=post action=/send>"
    "<div class=row><label>Destination<input name=destination maxlength=31 value=ALL placeholder='ALL or target Node ID'></label>"
    "<label>Emergency Type<select name=type><option>FLOOD</option><option>FIRE</option><option>MEDICAL</option><option>SECURITY</option><option>EVACUATION</option><option>TEST</option></select></label></div>"
    "<label>Priority<select name=priority><option>HIGH</option><option>NORMAL</option><option>LOW</option></select></label>"
    "<label>Message<textarea name=payload maxlength=159 placeholder='Short emergency message'></textarea></label>"
    "<button type=submit>Queue / Transmit Message</button></form><p class=muted>The portal sends through the SX1278 using GPIO 5/7/6/8/4/16 at 433 MHz.</p></section>"
    "<section class=card><h3>Recent Messages</h3><div id=messages class=muted>No messages yet.</div></section>"
    "<section class=card><h3>Portal</h3><p class=muted>Connect to this Wi-Fi when offline, then open http://192.168.4.1. Android/iOS captive checks are redirected here automatically.</p>"
    "<form method=post action=/reset onsubmit='return confirm(\"Factory reset this node and run setup again?\")'><button type=submit>Factory Reset Node</button></form></section>"
    "</main><script>"
    "async function load(){let s=await fetch('/api/status').then(r=>r.json());"
    "document.getElementById('status').innerHTML='Node <b>'+s.node+'</b> | '+s.name+' | '+s.location+' | Relay <b>'+s.relay+'</b> | AP <b>'+s.ssid+'</b> | Clients <b>'+s.clients+'</b>';"
    "let m=await fetch('/api/messages').then(r=>r.json());let box=document.getElementById('messages');"
    "box.innerHTML=m.length?m.map(x=>'<div class=msg><b>'+x.direction+' #'+x.id+' '+x.type+' '+x.priority+'</b><br>From: '+x.source+'<br>To: '+x.destination+'<br>'+x.payload+'<br><span class=muted>'+x.packet+'</span></div>').join(''):'No messages yet.'}"
    "load();setInterval(load,4000);</script></body></html>";

static const char SETUP_HTML[] =
    "<!doctype html><html><head><meta name=viewport content='width=device-width,initial-scale=1'>"
    "<title>Setup Barangay Mesh</title><style>"
    ":root{font-family:Arial,sans-serif;color:#17202a;background:#f5f7f9}"
    "body{margin:0}.top{background:#1f6f5b;color:white;padding:16px}.wrap{max-width:720px;margin:auto;padding:16px}"
    ".card{background:white;border:1px solid #d8dee4;border-radius:8px;padding:16px;margin:12px 0}"
    "label{display:block;font-weight:700;margin-top:12px}input,button{box-sizing:border-box;width:100%;font:inherit;padding:12px;margin-top:6px;border-radius:6px;border:1px solid #b8c0cc}"
    "button{background:#1f6f5b;color:white;border:0;font-weight:700;margin-top:16px}.muted{color:#5f6b7a;font-size:14px}"
    "</style></head><body><div class=top><div class=wrap><h2>First-Time Node Setup</h2>"
    "<p>Configure this universal mesh node once. All nodes still send, receive, and relay.</p></div></div>"
    "<main class=wrap><section class=card><form method=post action=/setup>"
    "<label>Node ID<input name=node_id maxlength=31 placeholder='Example: BRGY001, HH023, RELAY04' required></label>"
    "<label>Node Name<input name=node_name maxlength=31 placeholder='Example: Barangay Hall or House 23' required></label>"
    "<label>Location<input name=location maxlength=31 placeholder='Example: Purok 3, Chapel Roof' required></label>"
    "<label><input name=relay_enabled type=checkbox value=1 checked> Relay messages for the mesh</label>"
    "<button type=submit>Save Setup and Reboot</button></form>"
    "<p class=muted>Factory reset later by holding BOOT for 10 seconds during startup.</p></section></main></body></html>";

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

static emergency_message_t *next_message_slot(void)
{
    emergency_message_t *message;

    if (message_count < MAX_MESSAGES) {
        message = &messages[message_count++];
    } else {
        memmove(&messages[0], &messages[1], sizeof(messages[0]) * (MAX_MESSAGES - 1));
        message = &messages[MAX_MESSAGES - 1];
    }

    memset(message, 0, sizeof(*message));
    return message;
}

static bool packet_seen(const char *source, uint32_t id)
{
    for (size_t i = 0; i < seen_packet_count; i++) {
        if (seen_packets[i].id == id && strcmp(seen_packets[i].source, source) == 0) {
            return true;
        }
    }

    return false;
}

static void remember_packet(const char *source, uint32_t id)
{
    seen_packet_t *seen_packet;

    if (seen_packet_count < MAX_MESSAGES) {
        seen_packet = &seen_packets[seen_packet_count++];
    } else {
        memmove(&seen_packets[0], &seen_packets[1], sizeof(seen_packets[0]) * (MAX_MESSAGES - 1));
        seen_packet = &seen_packets[MAX_MESSAGES - 1];
    }

    seen_packet->id = id;
    copy_field(seen_packet->source, sizeof(seen_packet->source), source);
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
    copy_field(parsed->location, sizeof(parsed->location), fields[8]);
    copy_field(parsed->payload, sizeof(parsed->payload), fields[9]);
    return true;
}

static void build_forward_packet(const mesh_packet_t *parsed, char *packet, size_t packet_size)
{
    int next_hops = MAX(parsed->hops - 1, 0);

    snprintf(packet, packet_size, "BEMS|%lu|%.*s|%.*s|%.*s|%.*s|HOPS=%d|%.*s|%.*s|%.*s",
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
             31,
             parsed->location,
             120,
             parsed->payload);
}

static void store_received_packet(const char *packet, const mesh_packet_t *parsed, int rssi, int snr)
{
    emergency_message_t *message = next_message_slot();

    message->id = parsed->valid ? parsed->id : ++packet_counter;
    copy_field(message->direction, sizeof(message->direction), "RX");
    copy_field(message->source, sizeof(message->source), parsed->valid ? parsed->source : "UNKNOWN");
    copy_field(message->destination, sizeof(message->destination), parsed->valid ? parsed->destination : "UNKNOWN");
    copy_field(message->type, sizeof(message->type), parsed->valid ? parsed->type : "RECEIVED");
    copy_field(message->priority, sizeof(message->priority), parsed->valid ? parsed->priority : "NORMAL");
    copy_field(message->payload, sizeof(message->payload), parsed->valid ? parsed->payload : packet);
    snprintf(message->packet, sizeof(message->packet), "RSSI=%d SNR=%d | %.*s", rssi, snr, 250, packet);
}

static bool lora_transmit(const char *packet)
{
    size_t length = MIN(strlen(packet), (size_t)LORA_MAX_PAYLOAD);
    uint32_t start_tick = xTaskGetTickCount();

    if (!lora_ready) {
        ESP_LOGW(TAG, "SX1278 is not ready; packet kept in local log only");
        return false;
    }

    lora_set_mode(MODE_STDBY);
    lora_write_reg(REG_DIO_MAPPING_1, 0x40);
    lora_write_reg(REG_IRQ_FLAGS, 0xFF);
    lora_write_reg(REG_FIFO_ADDR_PTR, 0x00);
    lora_write_fifo((const uint8_t *)packet, length);
    lora_write_reg(REG_PAYLOAD_LENGTH, length);
    lora_set_mode(MODE_TX);

    while ((xTaskGetTickCount() - start_tick) < pdMS_TO_TICKS(5000)) {
        if ((lora_read_reg(REG_IRQ_FLAGS) & IRQ_TX_DONE_MASK) != 0) {
            lora_write_reg(REG_IRQ_FLAGS, 0xFF);
            lora_receive_mode();
            ESP_LOGI(TAG, "SX1278 TX done: %.*s", (int)length, packet);
            return true;
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }

    lora_write_reg(REG_IRQ_FLAGS, 0xFF);
    lora_receive_mode();
    ESP_LOGW(TAG, "SX1278 TX timeout");
    return false;
}

static void lora_rx_task(void *parameter)
{
    uint8_t payload[LORA_MAX_PAYLOAD + 1];

    while (true) {
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

                ESP_LOGI(TAG, "SX1278 RX RSSI=%d SNR=%d: %s", rssi, snr, (char *)payload);
                mesh_packet_t parsed;
                if (parse_mesh_packet((char *)payload, &parsed)) {
                    bool from_self = strcmp(parsed.source, node_id) == 0;
                    bool is_duplicate = packet_seen(parsed.source, parsed.id);
                    bool is_broadcast = strcmp(parsed.destination, "ALL") == 0;
                    bool is_for_me = strcmp(parsed.destination, node_id) == 0;

                    if (!from_self && !is_duplicate) {
                        remember_packet(parsed.source, parsed.id);

                        if (is_broadcast || is_for_me) {
                            store_received_packet((char *)payload, &parsed, rssi, snr);
                        }

                        if (node_config.relay_enabled && parsed.hops > 0 && !is_for_me) {
                            char forward_packet[PACKET_LEN];
                            build_forward_packet(&parsed, forward_packet, sizeof(forward_packet));
                            ESP_LOGI(TAG, "Relaying packet toward %s with %d hops left", parsed.destination, parsed.hops - 1);
                            lora_transmit(forward_packet);
                        }
                    }
                } else {
                    mesh_packet_t raw_packet = {0};
                    store_received_packet((char *)payload, &raw_packet, rssi, snr);
                }
            }
        }

        vTaskDelay(pdMS_TO_TICKS(25));
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
    };

    ESP_ERROR_CHECK(gpio_config(&reset_config));
    ESP_ERROR_CHECK(gpio_config(&dio0_config));

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
    lora_write_reg(REG_MODEM_CONFIG_1, 0x72);
    lora_write_reg(REG_MODEM_CONFIG_2, 0x74);
    lora_write_reg(REG_MODEM_CONFIG_3, 0x04);
    lora_write_reg(REG_PREAMBLE_MSB, 0x00);
    lora_write_reg(REG_PREAMBLE_LSB, 0x08);
    lora_write_reg(REG_SYNC_WORD, 0x12);
    lora_write_reg(REG_PA_CONFIG, 0x8F);

    lora_ready = true;
    lora_receive_mode();
    xTaskCreate(lora_rx_task, "lora_rx_task", 4096, NULL, 6, NULL);

    ESP_LOGI(TAG, "SX1278 ready on 433 MHz");
}

static void copy_field(char *destination, size_t destination_size, const char *source)
{
    size_t write_index = 0;

    if (destination_size == 0) {
        return;
    }

    while (*source != '\0' && write_index < destination_size - 1) {
        unsigned char character = (unsigned char)*source++;
        if (character >= 32 && character <= 126) {
            destination[write_index++] = (char)character;
        }
    }

    destination[write_index] = '\0';
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

static void config_set_defaults(void)
{
    node_config.configured = false;
    node_config.relay_enabled = true;
    copy_field(node_config.node_id, sizeof(node_config.node_id), node_id);
    copy_field(node_config.node_name, sizeof(node_config.node_name), "Unconfigured Node");
    copy_field(node_config.location, sizeof(node_config.location), "Unknown");
    copy_field(node_config.default_destination, sizeof(node_config.default_destination), "BRGY001");
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
    uint8_t relay_enabled = 1;

    config_set_defaults();

    if (nvs_open(CONFIG_NAMESPACE, NVS_READONLY, &handle) != ESP_OK) {
        apply_config_identity();
        return;
    }

    nvs_get_u8(handle, "configured", &configured);
    nvs_get_u8(handle, "relay", &relay_enabled);
    nvs_get_string_or_default(handle, "node_id", node_config.node_id, sizeof(node_config.node_id), node_id);
    nvs_get_string_or_default(handle, "node_name", node_config.node_name, sizeof(node_config.node_name), "Mesh Node");
    nvs_get_string_or_default(handle, "location", node_config.location, sizeof(node_config.location), "Unknown");
    nvs_get_string_or_default(handle, "default_dest", node_config.default_destination, sizeof(node_config.default_destination), "BRGY001");
    nvs_close(handle);

    node_config.configured = configured == 1;
    node_config.relay_enabled = relay_enabled == 1;
    apply_config_identity();

    ESP_LOGI(TAG, "Config loaded: configured=%d node=%s name=%s location=%s relay=%d",
             node_config.configured, node_config.node_id, node_config.node_name, node_config.location, node_config.relay_enabled);
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
        result = nvs_set_u8(handle, "relay", config->relay_enabled ? 1 : 0);
    }
    if (result == ESP_OK) {
        result = nvs_set_str(handle, "node_id", config->node_id);
    }
    if (result == ESP_OK) {
        result = nvs_set_str(handle, "node_name", config->node_name);
    }
    if (result == ESP_OK) {
        result = nvs_set_str(handle, "location", config->location);
    }
    if (result == ESP_OK) {
        result = nvs_set_str(handle, "default_dest", config->default_destination);
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

static int hex_value(char character)
{
    if (character >= '0' && character <= '9') {
        return character - '0';
    }
    if (character >= 'a' && character <= 'f') {
        return character - 'a' + 10;
    }
    if (character >= 'A' && character <= 'F') {
        return character - 'A' + 10;
    }
    return -1;
}

static void url_decode(char *value)
{
    char *read_ptr = value;
    char *write_ptr = value;

    while (*read_ptr != '\0') {
        if (*read_ptr == '+') {
            *write_ptr++ = ' ';
            read_ptr++;
        } else if (*read_ptr == '%' && isxdigit((unsigned char)read_ptr[1]) && isxdigit((unsigned char)read_ptr[2])) {
            *write_ptr++ = (char)((hex_value(read_ptr[1]) << 4) | hex_value(read_ptr[2]));
            read_ptr += 3;
        } else {
            *write_ptr++ = *read_ptr++;
        }
    }

    *write_ptr = '\0';
}

static bool form_value(const char *body, const char *key, char *output, size_t output_size)
{
    const size_t key_len = strlen(key);
    const char *cursor = body;

    while (cursor != NULL && *cursor != '\0') {
        if (strncmp(cursor, key, key_len) == 0 && cursor[key_len] == '=') {
            const char *value_start = cursor + key_len + 1;
            const char *value_end = strchr(value_start, '&');
            size_t value_len = value_end == NULL ? strlen(value_start) : (size_t)(value_end - value_start);
            size_t copy_len = MIN(value_len, output_size - 1);

            memcpy(output, value_start, copy_len);
            output[copy_len] = '\0';
            url_decode(output);
            return true;
        }

        cursor = strchr(cursor, '&');
        if (cursor != NULL) {
            cursor++;
        }
    }

    return false;
}

static void build_packet(emergency_message_t *message)
{
    snprintf(message->packet, sizeof(message->packet), "BEMS|%lu|%.*s|%.*s|%.*s|%.*s|HOPS=5|RELAY=%u|LOC=%.*s|%.*s",
             (unsigned long)message->id,
             31,
             node_id,
             31,
             message->destination,
             31,
             message->type,
             31,
             message->priority,
             node_config.relay_enabled ? 1 : 0,
             31,
             node_config.location,
             120,
             message->payload);
}

static void queue_message(const char *destination, const char *type, const char *priority, const char *payload)
{
    emergency_message_t *message = next_message_slot();

    message->id = ++packet_counter;
    copy_field(message->direction, sizeof(message->direction), "TX");
    copy_field(message->destination, sizeof(message->destination), destination);
    copy_field(message->type, sizeof(message->type), type);
    copy_field(message->priority, sizeof(message->priority), priority);
    copy_field(message->payload, sizeof(message->payload), payload);
    build_packet(message);

    ESP_LOGI(TAG, "LoRa TX pending: %s", message->packet);
    lora_transmit(message->packet);
}

static esp_err_t send_redirect(httpd_req_t *request, const char *location)
{
    httpd_resp_set_status(request, "302 Found");
    httpd_resp_set_hdr(request, "Location", location);
    httpd_resp_send(request, "", HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

static esp_err_t index_handler(httpd_req_t *request)
{
    httpd_resp_set_type(request, "text/html");

    if (!node_config.configured) {
        return httpd_resp_send(request, SETUP_HTML, HTTPD_RESP_USE_STRLEN);
    }

    return httpd_resp_send(request, INDEX_HTML, HTTPD_RESP_USE_STRLEN);
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
    form_value(body, "location", new_config.location, sizeof(new_config.location));
    form_value(body, "default_destination", new_config.default_destination, sizeof(new_config.default_destination));
    new_config.configured = true;
    new_config.relay_enabled = strstr(body, "relay_enabled=1") != NULL;

    if (new_config.node_id[0] == '\0') {
        copy_field(new_config.node_id, sizeof(new_config.node_id), node_id);
    }
    if (new_config.node_name[0] == '\0') {
        copy_field(new_config.node_name, sizeof(new_config.node_name), "Mesh Node");
    }
    if (new_config.location[0] == '\0') {
        copy_field(new_config.location, sizeof(new_config.location), "Unknown");
    }
    if (new_config.default_destination[0] == '\0') {
        copy_field(new_config.default_destination, sizeof(new_config.default_destination), "BRGY001");
    }

    ESP_ERROR_CHECK(save_node_config(&new_config));
    httpd_resp_set_type(request, "text/html");
    httpd_resp_send(request, "<!doctype html><html><body><h2>Setup saved.</h2><p>Node is restarting.</p></body></html>", HTTPD_RESP_USE_STRLEN);
    vTaskDelay(pdMS_TO_TICKS(500));
    esp_restart();
    return ESP_OK;
}

static esp_err_t reset_handler(httpd_req_t *request)
{
    erase_node_config();
    httpd_resp_set_type(request, "text/html");
    httpd_resp_send(request, "<!doctype html><html><body><h2>Factory reset complete.</h2><p>Node is restarting into setup mode.</p></body></html>", HTTPD_RESP_USE_STRLEN);
    vTaskDelay(pdMS_TO_TICKS(500));
    esp_restart();
    return ESP_OK;
}

static esp_err_t send_handler(httpd_req_t *request)
{
    char body[384] = {0};
    char destination[FIELD_LEN];
    char type[FIELD_LEN] = "TEST";
    char priority[FIELD_LEN] = "NORMAL";
    char payload[PAYLOAD_LEN] = "No message";
    int received = 0;

    while (received < request->content_len && received < (int)sizeof(body) - 1) {
        int ret = httpd_req_recv(request, body + received, MIN(request->content_len - received, (int)sizeof(body) - 1 - received));
        if (ret <= 0) {
            return ESP_FAIL;
        }
        received += ret;
    }

    copy_field(destination, sizeof(destination), node_config.default_destination);
    form_value(body, "destination", destination, sizeof(destination));
    form_value(body, "type", type, sizeof(type));
    form_value(body, "priority", priority, sizeof(priority));
    form_value(body, "payload", payload, sizeof(payload));

    queue_message(destination, type, priority, payload);
    return send_redirect(request, "/");
}

static esp_err_t status_handler(httpd_req_t *request)
{
    wifi_sta_list_t clients = {0};
    char response[384];

    esp_wifi_ap_get_sta_list(&clients);
    snprintf(response, sizeof(response),
             "{\"node\":\"%s\",\"name\":\"%s\",\"location\":\"%s\",\"ssid\":\"%s\",\"clients\":%u,\"messages\":%u,\"configured\":%s,\"relay\":%s}",
             node_id,
             node_config.node_name,
             node_config.location,
             ap_ssid,
             clients.num,
             (unsigned int)message_count,
             node_config.configured ? "true" : "false",
             node_config.relay_enabled ? "true" : "false");

    httpd_resp_set_type(request, "application/json");
    return httpd_resp_send(request, response, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t messages_handler(httpd_req_t *request)
{
    char response[1536];
    size_t offset = 0;

    offset += snprintf(response + offset, sizeof(response) - offset, "[");

    for (size_t i = 0; i < message_count && offset < sizeof(response); i++) {
        emergency_message_t *message = &messages[message_count - 1 - i];
        offset += snprintf(response + offset, sizeof(response) - offset,
                           "%s{\"id\":%lu,\"direction\":\"%s\",\"destination\":\"%s\",\"type\":\"%s\",\"priority\":\"%s\",\"payload\":\"%s\",\"packet\":\"%s\"}",
                           i == 0 ? "" : ",",
                           (unsigned long)message->id,
                           message->direction,
                           message->destination,
                           message->type,
                           message->priority,
                           message->payload,
                           message->packet);
    }

    snprintf(response + MIN(offset, sizeof(response) - 1), sizeof(response) - MIN(offset, sizeof(response) - 1), "]");
    httpd_resp_set_type(request, "application/json");
    return httpd_resp_send(request, response, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t captive_handler(httpd_req_t *request)
{
    return send_redirect(request, "/");
}

static void start_http_server(void)
{
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = HTTP_PORT;
    config.max_uri_handlers = 12;
    config.uri_match_fn = httpd_uri_match_wildcard;

    const httpd_uri_t routes[] = {
        {.uri = "/", .method = HTTP_GET, .handler = index_handler},
        {.uri = "/setup", .method = HTTP_POST, .handler = setup_handler},
        {.uri = "/reset", .method = HTTP_POST, .handler = reset_handler},
        {.uri = "/send", .method = HTTP_POST, .handler = send_handler},
        {.uri = "/api/status", .method = HTTP_GET, .handler = status_handler},
        {.uri = "/api/messages", .method = HTTP_GET, .handler = messages_handler},
        {.uri = "/generate_204", .method = HTTP_GET, .handler = captive_handler},
        {.uri = "/gen_204", .method = HTTP_GET, .handler = captive_handler},
        {.uri = "/hotspot-detect.html", .method = HTTP_GET, .handler = captive_handler},
        {.uri = "/connecttest.txt", .method = HTTP_GET, .handler = captive_handler},
        {.uri = "/ncsi.txt", .method = HTTP_GET, .handler = captive_handler},
        {.uri = "/*", .method = HTTP_GET, .handler = captive_handler},
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

    if (strlen(AP_PASSWORD) >= 8) {
        copy_field((char *)wifi_config.ap.password, sizeof(wifi_config.ap.password), AP_PASSWORD);
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

    init_identity();
    rgb_led_init();
    init_factory_reset_button();
    load_node_config();
    lora_init();
    start_wifi_ap();
    start_http_server();
    xTaskCreate(factory_reset_button_task, "factory_reset_button_task", 3072, NULL, 7, NULL);
    xTaskCreate(dns_task, "dns_task", 4096, NULL, 5, NULL);

    ESP_LOGI(TAG, "Universal mesh node portal ready as %s", node_id);
}
