#include "radio/lora_radio.h"

#include <string.h>

#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_err.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "bems_crypto.h"

static spi_device_handle_t lora_spi;
static const char *TAG = "barangay_mesh";
static radio_state_t lora_state = RADIO_STATE_UNINITIALIZED;
static SemaphoreHandle_t lora_dio0_semaphore;
static SemaphoreHandle_t lora_tx_done_semaphore;
static SemaphoreHandle_t lora_tx_mutex;
static QueueHandle_t lora_tx_queue;
static TaskHandle_t lora_tx_task_handle;
static bool (*lora_read_frame_callback)(uint8_t *payload, size_t *length, int *rssi, int *snr);

typedef struct {
    size_t length;
    lora_tx_priority_t priority;
    uint8_t packet[LORA_MAX_PAYLOAD];
} lora_tx_item_t;

static bool lora_read_raw_frame(uint8_t *payload, size_t *length, int *rssi, int *snr);
static esp_err_t lora_read_reg(uint8_t address, uint8_t *value);
static esp_err_t lora_write_reg(uint8_t address, uint8_t value);
static esp_err_t lora_write_fifo(const uint8_t *data, size_t length);
static esp_err_t lora_read_fifo(uint8_t *data, size_t length);
static bool lora_transmit_raw_frame(const uint8_t *frame, size_t length);
static bool lora_do_transmit_bytes(const uint8_t *packet, size_t packet_len);
static void lora_tx_task(void *parameter);
static void lora_enter_fault_internal(const char *reason);
static esp_err_t lora_radio_apply_defaults(void);
static bool lora_radio_bring_up(void);
static void lora_set_state(radio_state_t state);

static void lora_set_mode(uint8_t mode)
{
    (void)lora_write_reg(REG_OP_MODE, MODE_LONG_RANGE_MODE | mode);
}

static void lora_set_state(radio_state_t state)
{
    lora_state = state;
}

bool lora_channel_clear(void)
{
    if (!lora_radio_is_ready()) {
        return false;
    }
    lora_set_state(RADIO_STATE_CAD);
    for (int attempt = 0; attempt < 3; attempt++) {
        lora_set_mode(MODE_STDBY);
        if (lora_write_reg(REG_DIO_MAPPING_1, 0x80) != ESP_OK ||
            lora_write_reg(REG_IRQ_FLAGS, 0xFF) != ESP_OK) {
            lora_enter_fault_internal("CAD setup SPI failure");
            return false;
        }
        lora_set_mode(0x07); // CAD mode
        vTaskDelay(pdMS_TO_TICKS(10));

        uint8_t irq_flags = 0;
        if (lora_read_reg(REG_IRQ_FLAGS, &irq_flags) != ESP_OK ||
            lora_write_reg(REG_IRQ_FLAGS, 0xFF) != ESP_OK) {
            lora_enter_fault_internal("CAD read SPI failure");
            return false;
        }
        lora_set_mode(MODE_STDBY);

        if ((irq_flags & IRQ_CAD_DONE_MASK) != 0 && (irq_flags & IRQ_CAD_DETECTED_MASK) == 0) {
            lora_set_state(RADIO_STATE_RX);
            return true;
        }

        vTaskDelay(pdMS_TO_TICKS(20 + (attempt * 30)));
    }

    lora_set_state(RADIO_STATE_RX);
    return false;
}

static void lora_set_frequency(uint32_t frequency_hz)
{
    uint64_t frf = ((uint64_t)frequency_hz << 19) / 32000000;
    (void)lora_write_reg(REG_FRF_MSB, (uint8_t)(frf >> 16));
    (void)lora_write_reg(REG_FRF_MID, (uint8_t)(frf >> 8));
    (void)lora_write_reg(REG_FRF_LSB, (uint8_t)(frf >> 0));
}

static void lora_receive_mode(void)
{
    (void)lora_write_reg(REG_DIO_MAPPING_1, 0x00);
    (void)lora_write_reg(REG_IRQ_FLAGS, 0xFF);
    lora_set_mode(MODE_RX_CONTINUOUS);
    lora_set_state(RADIO_STATE_RX);
}

static esp_err_t lora_radio_apply_defaults(void)
{
    uint8_t version = 0;

    if (lora_read_reg(REG_VERSION, &version) != ESP_OK || version != 0x12) {
        return ESP_FAIL;
    }

    lora_set_mode(MODE_SLEEP);
    vTaskDelay(pdMS_TO_TICKS(10));
    lora_set_frequency(LORA_FREQUENCY_HZ);
    if (lora_write_reg(REG_FIFO_TX_BASE_ADDR, 0x00) != ESP_OK ||
        lora_write_reg(REG_FIFO_RX_BASE_ADDR, 0x00) != ESP_OK ||
        lora_read_reg(REG_LNA, &version) != ESP_OK ||
        lora_write_reg(REG_LNA, version | 0x03) != ESP_OK ||
        lora_write_reg(REG_MODEM_CONFIG_1, LORA_MODEM_CONFIG_1) != ESP_OK ||
        lora_write_reg(REG_MODEM_CONFIG_2, LORA_MODEM_CONFIG_2) != ESP_OK ||
        lora_write_reg(REG_MODEM_CONFIG_3, LORA_MODEM_CONFIG_3) != ESP_OK ||
        lora_write_reg(REG_PREAMBLE_MSB, 0x00) != ESP_OK ||
        lora_write_reg(REG_PREAMBLE_LSB, 0x08) != ESP_OK ||
        lora_write_reg(REG_SYNC_WORD, 0x12) != ESP_OK ||
        lora_write_reg(REG_PA_CONFIG, 0x8F) != ESP_OK) {
        return ESP_FAIL;
    }

    return ESP_OK;
}

static bool lora_radio_bring_up(void)
{
    if (lora_radio_apply_defaults() != ESP_OK) {
        return false;
    }

    lora_receive_mode();
    return true;
}

static void IRAM_ATTR lora_dio0_isr_handler(void *arg)
{
    BaseType_t high_priority_task_woken = pdFALSE;
    uint8_t irq_flags = 0;
    if (lora_read_reg(REG_IRQ_FLAGS, &irq_flags) != ESP_OK) {
        return;
    }
    SemaphoreHandle_t semaphore = (irq_flags & IRQ_TX_DONE_MASK) != 0 ? lora_tx_done_semaphore : lora_dio0_semaphore;

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

        if (lora_state == RADIO_STATE_RX && lora_read_frame_callback != NULL) {
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

    lora_set_state(RADIO_STATE_INITIALIZING);
    ESP_ERROR_CHECK(spi_bus_initialize(LORA_SPI_HOST, &bus_config, SPI_DMA_CH_AUTO));
    ESP_ERROR_CHECK(spi_bus_add_device(LORA_SPI_HOST, &device_config, &lora_spi));

    if (lora_radio_reset() != ESP_OK || !lora_radio_bring_up()) {
        lora_enter_fault_internal("Failed to initialize SX1278");
        return;
    }

    lora_read_frame_callback = lora_read_raw_frame;
    xTaskCreate(lora_rx_task, "lora_rx_task", 4096, NULL, 6, NULL);
    lora_tx_mutex = xSemaphoreCreateMutex();
    lora_tx_queue = xQueueCreate(8, sizeof(lora_tx_item_t));
    if (lora_tx_mutex != NULL && lora_tx_queue != NULL) {
        xTaskCreate(lora_tx_task, "lora_tx_task", 4096, NULL, 7, &lora_tx_task_handle);
    }

    lora_set_state(RADIO_STATE_RX);
    ESP_LOGI(TAG, "SX1278 ready on 433 MHz");
}

bool lora_transmit(const char *packet)
{
    uint8_t frame[LORA_MAX_PAYLOAD];
    size_t length = 0;

    if (!lora_radio_is_ready()) {
        ESP_LOGW(TAG, "SX1278 is not ready; packet kept in local log only");
        return false;
    }

    if (!bems_encrypt_packet(packet, frame, sizeof(frame), &length)) {
        ESP_LOGW(TAG, "Failed to encrypt LoRa packet");
        return false;
    }

    return lora_radio_submit(frame, length, LORA_TX_PRIORITY_NORMAL);
}

bool lora_transmit_bytes(const uint8_t *packet, size_t packet_len)
{
    return lora_radio_submit(packet, packet_len, LORA_TX_PRIORITY_NORMAL);
}

bool lora_radio_is_ready(void)
{
    return lora_state == RADIO_STATE_RX;
}

radio_state_t lora_radio_get_state(void)
{
    return lora_state;
}

void lora_radio_enter_fault(void)
{
    lora_enter_fault_internal("manual fault");
}

bool lora_radio_health_check(void)
{
    uint8_t version = 0;

    if (lora_state == RADIO_STATE_UNINITIALIZED || lora_state == RADIO_STATE_FAULT) {
        return false;
    }
    if (lora_read_reg(REG_VERSION, &version) != ESP_OK || version != 0x12) {
        lora_enter_fault_internal("health check failed");
        return false;
    }
    return true;
}

esp_err_t lora_radio_reset(void)
{
    lora_set_state(RADIO_STATE_RECOVERING);
    gpio_set_level(LORA_RST_GPIO, 0);
    vTaskDelay(pdMS_TO_TICKS(10));
    gpio_set_level(LORA_RST_GPIO, 1);
    vTaskDelay(pdMS_TO_TICKS(20));
    return ESP_OK;
}

bool lora_radio_recover(void)
{
    lora_enter_fault_internal("recovering");
    if (lora_radio_reset() != ESP_OK) {
        lora_set_state(RADIO_STATE_FAULT);
        return false;
    }
    if (!lora_radio_bring_up()) {
        lora_set_state(RADIO_STATE_FAULT);
        return false;
    }
    lora_set_state(RADIO_STATE_RX);
    return true;
}

bool lora_radio_submit(const uint8_t *packet, size_t length, lora_tx_priority_t priority)
{
    lora_tx_item_t item = {0};

    if (lora_state != RADIO_STATE_RX) {
        ESP_LOGW(TAG, "SX1278 is not ready; packet kept in local log only");
        return false;
    }
    if (packet == NULL || length == 0 || length > sizeof(item.packet)) {
        return false;
    }

    item.length = length;
    item.priority = priority;
    memcpy(item.packet, packet, length);

    if (lora_tx_queue == NULL) {
        return lora_do_transmit_bytes(item.packet, item.length);
    }

    if (xQueueSend(lora_tx_queue, &item, 0) != pdTRUE) {
        ESP_LOGW(TAG, "LoRa TX queue full");
        return false;
    }
    return true;
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

static esp_err_t lora_read_reg(uint8_t address, uint8_t *value)
{
    if (value == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    return lora_transfer(address & 0x7F, NULL, value, 1);
}

static esp_err_t lora_write_reg(uint8_t address, uint8_t value)
{
    return lora_transfer(address | 0x80, &value, NULL, 1);
}

static esp_err_t lora_write_fifo(const uint8_t *data, size_t length)
{
    return lora_transfer(REG_FIFO | 0x80, data, NULL, length);
}

static esp_err_t lora_read_fifo(uint8_t *data, size_t length)
{
    return lora_transfer(REG_FIFO & 0x7F, NULL, data, length);
}

static bool lora_read_raw_frame(uint8_t *payload, size_t *length, int *rssi, int *snr)
{
    uint8_t flags;
    uint8_t frame_length;
    uint8_t current_addr;

    if (lora_read_reg(REG_IRQ_FLAGS, &flags) != ESP_OK) {
        lora_enter_fault_internal("RX IRQ read failed");
        return false;
    }
    if ((flags & IRQ_RX_DONE_MASK) == 0) {
        return false;
    }

    if (lora_write_reg(REG_IRQ_FLAGS, 0xFF) != ESP_OK) {
        lora_enter_fault_internal("RX IRQ clear failed");
        return false;
    }

    if ((flags & IRQ_PAYLOAD_CRC_ERROR_MASK) != 0) {
        return false;
    }

    if (lora_read_reg(REG_RX_NB_BYTES, &frame_length) != ESP_OK ||
        lora_read_reg(REG_FIFO_RX_CURRENT_ADDR, &current_addr) != ESP_OK ||
        lora_read_reg(REG_PKT_RSSI_VALUE, &flags) != ESP_OK) {
        return false;
    }
    *rssi = (int)flags - 164;
    if (lora_read_reg(REG_PKT_SNR_VALUE, &flags) != ESP_OK) {
        return false;
    }
    *snr = ((int8_t)flags) / 4;

    if (lora_write_reg(REG_FIFO_ADDR_PTR, current_addr) != ESP_OK ||
        lora_read_fifo(payload, frame_length) != ESP_OK) {
        return false;
    }
    payload[frame_length] = '\0';
    *length = frame_length;
    return true;
}

static bool lora_transmit_raw_frame(const uint8_t *frame, size_t length)
{
    return lora_write_reg(REG_DIO_MAPPING_1, 0x40) == ESP_OK &&
           lora_write_reg(REG_IRQ_FLAGS, 0xFF) == ESP_OK &&
           lora_write_reg(REG_FIFO_ADDR_PTR, 0x00) == ESP_OK &&
           lora_write_fifo(frame, length) == ESP_OK &&
           lora_write_reg(REG_PAYLOAD_LENGTH, (uint8_t)length) == ESP_OK;
}

static bool lora_do_transmit_bytes(const uint8_t *packet, size_t packet_len)
{
    if (lora_tx_done_semaphore == NULL) {
        ESP_LOGW(TAG, "LoRa TX done semaphore is not ready");
        return false;
    }

    if (lora_tx_mutex != NULL) {
        xSemaphoreTake(lora_tx_mutex, portMAX_DELAY);
    }

    if (!lora_channel_clear()) {
        if (lora_tx_mutex != NULL) {
            xSemaphoreGive(lora_tx_mutex);
        }
        ESP_LOGW(TAG, "Channel busy; TX skipped");
        return false;
    }

    xSemaphoreTake(lora_tx_done_semaphore, 0);
    lora_set_state(RADIO_STATE_TX);
    if (!lora_transmit_raw_frame(packet, packet_len)) {
        lora_receive_mode();
        if (lora_tx_mutex != NULL) {
            xSemaphoreGive(lora_tx_mutex);
        }
        lora_enter_fault_internal("TX frame write failed");
        (void)lora_radio_recover();
        return false;
    }

    lora_set_mode(MODE_TX);
    if (xSemaphoreTake(lora_tx_done_semaphore, pdMS_TO_TICKS(5000)) == pdTRUE) {
        lora_receive_mode();
        if (lora_tx_mutex != NULL) {
            xSemaphoreGive(lora_tx_mutex);
        }
        ESP_LOGI(TAG, "SX1278 TX done: %u bytes", (unsigned int)packet_len);
        return true;
    }

    lora_enter_fault_internal("TX timeout");
    (void)lora_radio_recover();
    lora_receive_mode();
    if (lora_tx_mutex != NULL) {
        xSemaphoreGive(lora_tx_mutex);
    }
    ESP_LOGW(TAG, "SX1278 TX timeout");
    return false;
}

static void lora_tx_task(void *parameter)
{
    lora_tx_item_t item;

    (void)parameter;
    while (true) {
        if (lora_tx_queue == NULL || xQueueReceive(lora_tx_queue, &item, portMAX_DELAY) != pdTRUE) {
            continue;
        }
        if (item.priority == LORA_TX_PRIORITY_HIGH) {
            (void)lora_do_transmit_bytes(item.packet, item.length);
        } else {
            (void)lora_do_transmit_bytes(item.packet, item.length);
        }
    }
}

static void lora_enter_fault_internal(const char *reason)
{
    lora_set_state(RADIO_STATE_FAULT);
    if (reason != NULL) {
        ESP_LOGE(TAG, "LoRa radio fault: %s", reason);
    }
}
