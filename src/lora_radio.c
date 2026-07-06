#include "lora_radio.h"

#include <string.h>

#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "bems_crypto.h"
#include "mesh_protocol.h"

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

static const char *TAG = "lora_radio";
static spi_device_handle_t lora_spi;
static bool lora_ready;

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

static bool lora_channel_clear(void)
{
    for (int attempt = 0; attempt < 3; attempt++) {
        lora_write_reg(REG_DIO_MAPPING_1, 0x80);
        lora_write_reg(REG_IRQ_FLAGS, 0xFF);
        lora_write_reg(REG_IRQ_FLAGS_1, 0xFF);
        lora_write_reg(REG_OP_MODE, MODE_LONG_RANGE_MODE | 0x07);
        vTaskDelay(pdMS_TO_TICKS(10));

        uint8_t irq_flags_1 = lora_read_reg(REG_IRQ_FLAGS_1);
        lora_write_reg(REG_IRQ_FLAGS, 0xFF);
        lora_write_reg(REG_IRQ_FLAGS_1, 0xFF);
        lora_write_reg(REG_OP_MODE, MODE_LONG_RANGE_MODE | MODE_STDBY);

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

void lora_receive_mode(void)
{
    lora_write_reg(REG_DIO_MAPPING_1, 0x00);
    lora_write_reg(REG_IRQ_FLAGS, 0xFF);
    lora_write_reg(REG_OP_MODE, MODE_LONG_RANGE_MODE | MODE_RX_CONTINUOUS);
}

void lora_init(void)
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
    gpio_set_level(LORA_RST_GPIO, 0);
    vTaskDelay(pdMS_TO_TICKS(10));
    gpio_set_level(LORA_RST_GPIO, 1);
    vTaskDelay(pdMS_TO_TICKS(20));

    ESP_ERROR_CHECK(spi_bus_initialize(LORA_SPI_HOST, &bus_config, SPI_DMA_CH_AUTO));
    ESP_ERROR_CHECK(spi_bus_add_device(LORA_SPI_HOST, &device_config, &lora_spi));

    if (lora_read_reg(REG_VERSION) != 0x12) {
        ESP_LOGE(TAG, "SX1278 not detected");
        return;
    }

    lora_write_reg(REG_OP_MODE, MODE_LONG_RANGE_MODE | MODE_SLEEP);
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
}

bool lora_transmit(const char *packet)
{
    uint8_t frame[LORA_MAX_PAYLOAD];
    size_t length = 0;

    if (!lora_ready) {
        return false;
    }

    if (!bems_encrypt_packet(packet, frame, sizeof(frame), &length)) {
        return false;
    }

    if (!lora_channel_clear()) {
        return false;
    }

    lora_write_reg(REG_OP_MODE, MODE_LONG_RANGE_MODE | MODE_STDBY);
    lora_write_reg(REG_DIO_MAPPING_1, 0x40);
    lora_write_reg(REG_IRQ_FLAGS, 0xFF);
    lora_write_reg(REG_FIFO_ADDR_PTR, 0x00);
    lora_write_fifo(frame, length);
    lora_write_reg(REG_PAYLOAD_LENGTH, length);
    lora_write_reg(REG_OP_MODE, MODE_LONG_RANGE_MODE | MODE_TX);
    lora_receive_mode();
    return true;
}
