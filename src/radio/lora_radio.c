#include "radio/lora_radio.h"

#include <string.h>

#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_err.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "bems_crypto.h"

static spi_device_handle_t lora_spi;
static const char *TAG = "barangay_mesh";
static bool lora_ready;
static volatile bool radio_in_tx;
static SemaphoreHandle_t lora_dio0_semaphore;
static SemaphoreHandle_t lora_tx_done_semaphore;
static bool (*lora_read_frame_callback)(uint8_t *payload, size_t *length, int *rssi, int *snr);

static bool lora_read_raw_frame(uint8_t *payload, size_t *length, int *rssi, int *snr);
static uint8_t lora_read_reg(uint8_t address);
static void lora_write_reg(uint8_t address, uint8_t value);
static void lora_write_fifo(const uint8_t *data, size_t length);
static void lora_read_fifo(uint8_t *data, size_t length);
static bool lora_transmit_raw_frame(const uint8_t *frame, size_t length);

static void lora_set_mode(uint8_t mode)
{
    lora_write_reg(REG_OP_MODE, MODE_LONG_RANGE_MODE | mode);
}

bool lora_channel_clear(void)
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

static void lora_rx_task(void *parameter)
{
    uint8_t payload[LORA_MAX_PAYLOAD + 1];

    while (true) {
        if (lora_dio0_semaphore != NULL) {
            xSemaphoreTake(lora_dio0_semaphore, portMAX_DELAY);
        }

        if (lora_ready && lora_read_frame_callback != NULL) {
            size_t length = 0;
            int rssi = 0;
            int snr = 0;

            if (lora_read_frame_callback(payload, &length, &rssi, &snr)) {
                lora_handle_rx_packet(payload, length, rssi, snr);
            }
        }
    }
}

void lora_radio_init(void)
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
    lora_read_frame_callback = lora_read_raw_frame;
    xTaskCreate(lora_rx_task, "lora_rx_task", 4096, NULL, 6, NULL);

    ESP_LOGI(TAG, "SX1278 ready on 433 MHz");
}

bool lora_transmit(const char *packet)
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
    radio_in_tx = true;
    if (lora_transmit_raw_frame(frame, length)) {
        lora_write_reg(REG_IRQ_FLAGS, 0xFF);
        lora_set_mode(MODE_TX);

        if (xSemaphoreTake(lora_tx_done_semaphore, pdMS_TO_TICKS(5000)) == pdTRUE) {
            radio_in_tx = false;
            lora_write_reg(REG_IRQ_FLAGS, 0xFF);
            lora_receive_mode();
            ESP_LOGI(TAG, "SX1278 encrypted TX done: %u bytes", (unsigned int)length);
            return true;
        }
    }

    radio_in_tx = false;
    lora_write_reg(REG_IRQ_FLAGS, 0xFF);
    lora_receive_mode();
    ESP_LOGW(TAG, "SX1278 TX timeout");
    return false;
}

bool lora_radio_is_ready(void)
{
    return lora_ready;
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

static bool lora_read_raw_frame(uint8_t *payload, size_t *length, int *rssi, int *snr)
{
    uint8_t flags;
    uint8_t frame_length;
    uint8_t current_addr;

    if ((lora_read_reg(REG_IRQ_FLAGS) & IRQ_RX_DONE_MASK) == 0) {
        return false;
    }

    flags = lora_read_reg(REG_IRQ_FLAGS);
    lora_write_reg(REG_IRQ_FLAGS, 0xFF);

    if ((flags & IRQ_PAYLOAD_CRC_ERROR_MASK) != 0) {
        return false;
    }

    frame_length = lora_read_reg(REG_RX_NB_BYTES);
    current_addr = lora_read_reg(REG_FIFO_RX_CURRENT_ADDR);
    *rssi = (int)lora_read_reg(REG_PKT_RSSI_VALUE) - 164;
    *snr = ((int8_t)lora_read_reg(REG_PKT_SNR_VALUE)) / 4;

    lora_write_reg(REG_FIFO_ADDR_PTR, current_addr);
    lora_read_fifo(payload, frame_length);
    payload[frame_length] = '\0';
    *length = frame_length;
    return true;
}

static bool lora_transmit_raw_frame(const uint8_t *frame, size_t length)
{
    lora_write_reg(REG_DIO_MAPPING_1, 0x40);
    lora_write_reg(REG_IRQ_FLAGS, 0xFF);
    lora_write_reg(REG_FIFO_ADDR_PTR, 0x00);
    lora_write_fifo(frame, length);
    lora_write_reg(REG_PAYLOAD_LENGTH, (uint8_t)length);
    return true;
}
