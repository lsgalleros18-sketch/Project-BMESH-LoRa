#include "radio/lora_radio.h"

#include <string.h>

#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_random.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "bems_crypto.h"

static spi_device_handle_t lora_spi;
static const char *TAG = "barangay_mesh";
static radio_state_t lora_state = RADIO_STATE_UNINITIALIZED;
static SemaphoreHandle_t lora_dio0_semaphore;
static SemaphoreHandle_t lora_tx_mutex;
static QueueHandle_t lora_tx_request_queues[3];
static QueueHandle_t lora_tx_result_queue;
static TaskHandle_t lora_tx_task_handle;
static bool (*lora_read_frame_callback)(uint8_t *payload, size_t *length, int *rssi, int *snr);
static uint8_t lora_recovery_attempts;

static bool lora_read_raw_frame(uint8_t *payload, size_t *length, int *rssi, int *snr);
static esp_err_t lora_read_reg(uint8_t address, uint8_t *value);
static esp_err_t lora_write_reg(uint8_t address, uint8_t value);
static esp_err_t lora_write_fifo(const uint8_t *data, size_t length);
static esp_err_t lora_read_fifo(uint8_t *data, size_t length);
static bool lora_transmit_raw_frame(const uint8_t *frame, size_t length);
static bool lora_encrypt_air_frame(const uint8_t *plain_packet, size_t plain_len, uint8_t *air_frame, size_t air_frame_size, size_t *air_len);
static void lora_tx_task(void *parameter);
static void lora_enter_fault_internal(const char *reason);
static esp_err_t lora_radio_apply_defaults(void);
static bool lora_radio_bring_up(void);
static void lora_set_state(radio_state_t state);
static lora_tx_result_code_t lora_execute_tx(const lora_tx_request_t *request, lora_tx_result_t *result);

static esp_err_t lora_set_mode(uint8_t mode)
{
    return lora_write_reg(REG_OP_MODE, MODE_LONG_RANGE_MODE | mode);
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
        if (lora_set_mode(MODE_STDBY) != ESP_OK) {
            lora_enter_fault_internal("CAD standby failed");
            return false;
        }
        if (lora_write_reg(REG_DIO_MAPPING_1, 0x80) != ESP_OK ||
            lora_write_reg(REG_IRQ_FLAGS, 0xFF) != ESP_OK) {
            lora_enter_fault_internal("CAD setup SPI failure");
            return false;
        }
        if (lora_set_mode(0x07) != ESP_OK) { // CAD mode
            lora_enter_fault_internal("CAD mode failed");
            return false;
        }
        vTaskDelay(pdMS_TO_TICKS(10));

        uint8_t irq_flags = 0;
        if (lora_read_reg(REG_IRQ_FLAGS, &irq_flags) != ESP_OK ||
            lora_write_reg(REG_IRQ_FLAGS, 0xFF) != ESP_OK) {
            lora_enter_fault_internal("CAD read SPI failure");
            return false;
        }
        if (lora_set_mode(MODE_STDBY) != ESP_OK) {
            lora_enter_fault_internal("CAD exit standby failed");
            return false;
        }

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
    (void)lora_set_mode(MODE_RX_CONTINUOUS);
    lora_set_state(RADIO_STATE_RX);
}

static esp_err_t lora_radio_apply_defaults(void)
{
    uint8_t version = 0;

    if (lora_read_reg(REG_VERSION, &version) != ESP_OK || version != 0x12) {
        return ESP_FAIL;
    }

    if (lora_set_mode(MODE_SLEEP) != ESP_OK) {
        return ESP_FAIL;
    }
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
    (void)arg;
    if (lora_dio0_semaphore != NULL) {
        xSemaphoreGiveFromISR(lora_dio0_semaphore, &high_priority_task_woken);
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

        uint8_t irq_flags = 0;
        if (lora_read_reg(REG_IRQ_FLAGS, &irq_flags) != ESP_OK) {
            continue;
        }
        if ((irq_flags & IRQ_TX_DONE_MASK) != 0 && lora_state != RADIO_STATE_RX) {
            continue;
        }
        if ((irq_flags & IRQ_TX_DONE_MASK) != 0) {
            (void)lora_write_reg(REG_IRQ_FLAGS, IRQ_TX_DONE_MASK);
            continue;
        }
        if ((irq_flags & IRQ_RX_DONE_MASK) == 0) {
            continue;
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

    lora_tx_mutex = xSemaphoreCreateMutex();
    if (lora_tx_mutex == NULL) {
        ESP_LOGE(TAG, "Failed to create LoRa SPI mutex");
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
    for (size_t i = 0; i < 3; i++) {
        lora_tx_request_queues[i] = xQueueCreate(16, sizeof(lora_tx_request_t));
    }
    lora_tx_result_queue = xQueueCreate(16, sizeof(lora_tx_result_t));
    xTaskCreate(lora_rx_task, "lora_rx_task", 4096, NULL, 6, NULL);
    if (lora_tx_request_queues[0] != NULL && lora_tx_request_queues[1] != NULL &&
        lora_tx_request_queues[2] != NULL && lora_tx_result_queue != NULL) {
        xTaskCreate(lora_tx_task, "lora_tx_task", 4096, NULL, 7, &lora_tx_task_handle);
    }

    lora_set_state(RADIO_STATE_RX);
    ESP_LOGI(TAG, "SX1278 ready on 433 MHz");
}

bool lora_transmit(const char *packet)
{
    if (packet == NULL) {
        return false;
    }

    return lora_radio_submit((const uint8_t *)packet, strlen(packet), LORA_TX_PRIORITY_NORMAL);
}

bool lora_transmit_bytes(const uint8_t *packet, size_t packet_len)
{
    return lora_transmit_bytes_priority(packet, packet_len, LORA_TX_PRIORITY_NORMAL);
}

bool lora_transmit_bytes_priority(const uint8_t *packet, size_t packet_len, lora_tx_priority_t priority)
{
    return lora_radio_submit(packet, packet_len, priority);
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
    if (lora_recovery_attempts >= 3) {
        lora_set_state(RADIO_STATE_FAULT);
        ESP_LOGE(TAG, "LoRa recovery limit reached");
        return false;
    }
    lora_recovery_attempts++;
    lora_enter_fault_internal("recovering");
    if (lora_radio_reset() != ESP_OK) {
        lora_set_state(RADIO_STATE_FAULT);
        return false;
    }
    if (!lora_radio_bring_up()) {
        lora_set_state(RADIO_STATE_FAULT);
        return false;
    }
    lora_recovery_attempts = 0;
    lora_set_state(RADIO_STATE_RX);
    return true;
}

bool lora_radio_submit(const uint8_t *packet, size_t length, lora_tx_priority_t priority)
{
    lora_tx_request_t request = {0};

    if (lora_state != RADIO_STATE_RX) {
        ESP_LOGW(TAG, "SX1278 is not ready; packet kept in local log only");
        return false;
    }
    if (packet == NULL || length == 0 || length > BEMS_MAX_PLAINTEXT || length > sizeof(request.packet)) {
        return false;
    }

    request.request_id = esp_random();
    request.packet_len = length;
    request.priority = priority;
    request.submitted_at_ms = (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
    request.require_result = true;
    memcpy(request.packet, packet, length);

    return lora_tx_submit(&request) == ESP_OK;
}

esp_err_t lora_tx_submit(const lora_tx_request_t *request)
{
    if (request == NULL || request->packet_len == 0 ||
        request->packet_len > BEMS_MAX_PLAINTEXT ||
        request->packet_len > sizeof(((lora_tx_request_t *)0)->packet)) {
        return ESP_ERR_INVALID_ARG;
    }
    size_t queue_index = request->priority == LORA_TX_PRIORITY_HIGH ? 0 :
                         request->priority == LORA_TX_PRIORITY_LOW ? 2 : 1;
    if (lora_tx_request_queues[queue_index] == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    if (xQueueSend(lora_tx_request_queues[queue_index], request, 0) != pdTRUE) {
        ESP_LOGW(TAG, "LoRa TX queue full");
        return ESP_ERR_TIMEOUT;
    }
    if (lora_tx_task_handle != NULL) xTaskNotifyGive(lora_tx_task_handle);
    return ESP_OK;
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

    if (lora_tx_mutex != NULL) {
        xSemaphoreTake(lora_tx_mutex, portMAX_DELAY);
    }
    esp_err_t result = spi_device_transmit(lora_spi, &transaction);
    if (lora_tx_mutex != NULL) {
        xSemaphoreGive(lora_tx_mutex);
    }
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

static bool lora_encrypt_air_frame(const uint8_t *plain_packet, size_t plain_len, uint8_t *air_frame, size_t air_frame_size, size_t *air_len)
{
    if (plain_packet == NULL || air_frame == NULL || air_len == NULL || plain_len == 0 || plain_len > BEMS_MAX_PLAINTEXT) {
        return false;
    }
    return bems_encrypt_frame(plain_packet, plain_len, air_frame, air_frame_size, air_len);
}

static void lora_tx_task(void *parameter)
{
    lora_tx_request_t request;
    lora_tx_result_t result;
    uint8_t high_streak = 0;
    uint8_t normal_streak = 0;

    (void)parameter;
    while (true) {
        bool high_waiting = uxQueueMessagesWaiting(lora_tx_request_queues[0]) > 0;
        bool normal_waiting = uxQueueMessagesWaiting(lora_tx_request_queues[1]) > 0;
        bool low_waiting = uxQueueMessagesWaiting(lora_tx_request_queues[2]) > 0;
        QueueHandle_t selected_queue = NULL;
        if (low_waiting && (high_streak >= 4 || normal_streak >= 3)) {
            selected_queue = lora_tx_request_queues[2];
            high_streak = normal_streak = 0;
        } else if (normal_waiting && high_streak >= 4) {
            selected_queue = lora_tx_request_queues[1];
            high_streak = 0;
            normal_streak++;
        } else if (high_waiting) {
            selected_queue = lora_tx_request_queues[0];
            high_streak++;
        } else if (normal_waiting) {
            selected_queue = lora_tx_request_queues[1];
            high_streak = 0;
            normal_streak++;
        } else if (low_waiting) {
            selected_queue = lora_tx_request_queues[2];
            high_streak = normal_streak = 0;
        }
        if (selected_queue == NULL) {
            (void)ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
            continue;
        }
        if (xQueueReceive(selected_queue, &request, 0) != pdTRUE) {
            continue;
        }
        memset(&result, 0, sizeof(result));
        result.request_id = request.request_id;
        result.message_id = request.message_id;
        result.started_at_ms = (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
        result.result = lora_execute_tx(&request, &result);
        result.completed_at_ms = (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
        if (lora_tx_result_queue != NULL && xQueueSend(lora_tx_result_queue, &result, pdMS_TO_TICKS(100)) != pdTRUE) {
            ESP_LOGW(TAG, "Dropped TX result request_id=%lu", (unsigned long)result.request_id);
        }
    }
}

bool lora_tx_result_receive(lora_tx_result_t *result, TickType_t timeout_ticks)
{
    if (result == NULL || lora_tx_result_queue == NULL) {
        return false;
    }
    return xQueueReceive(lora_tx_result_queue, result, timeout_ticks) == pdTRUE;
}

static lora_tx_result_code_t lora_execute_tx(const lora_tx_request_t *request, lora_tx_result_t *result)
{
    uint8_t air_frame[LORA_MAX_PAYLOAD];
    size_t air_len = 0;

    if (request == NULL || result == NULL) {
        return LORA_TX_RESULT_UNKNOWN;
    }
    if (!lora_radio_is_ready()) {
        return LORA_TX_RESULT_RADIO_FAULT;
    }
    if (!lora_encrypt_air_frame(request->packet, request->packet_len, air_frame, sizeof(air_frame), &air_len)) {
        ESP_LOGW(TAG, "Failed to encrypt LoRa TX frame (%u plaintext bytes)", (unsigned int)request->packet_len);
        return LORA_TX_RESULT_ABORTED;
    }
    if (!lora_channel_clear()) {
        return LORA_TX_RESULT_CHANNEL_BUSY;
    }
    xSemaphoreTake(lora_dio0_semaphore, 0);
    lora_set_state(RADIO_STATE_TX);
    if (!lora_transmit_raw_frame(air_frame, air_len)) {
        lora_receive_mode();
        lora_enter_fault_internal("TX frame write failed");
        (void)lora_radio_recover();
        return LORA_TX_RESULT_SPI_ERROR;
    }
    if (lora_set_mode(MODE_TX) != ESP_OK) {
        lora_enter_fault_internal("TX mode set failed");
        (void)lora_radio_recover();
        return LORA_TX_RESULT_SPI_ERROR;
    }
    if (xSemaphoreTake(lora_dio0_semaphore, pdMS_TO_TICKS(5000)) == pdTRUE) {
        uint8_t irq_flags = 0;
        if (lora_read_reg(REG_IRQ_FLAGS, &irq_flags) != ESP_OK || (irq_flags & IRQ_TX_DONE_MASK) == 0) {
            lora_enter_fault_internal("unexpected DIO0 event during TX");
            (void)lora_radio_recover();
            return LORA_TX_RESULT_SPI_ERROR;
        }
        (void)lora_write_reg(REG_IRQ_FLAGS, IRQ_TX_DONE_MASK);
        lora_receive_mode();
        result->airtime_ms = 0;
        result->rssi = 0;
        result->snr = 0;
        return LORA_TX_RESULT_SUCCESS;
    }
    lora_enter_fault_internal("TX timeout");
    (void)lora_radio_recover();
    lora_receive_mode();
    return LORA_TX_RESULT_TIMEOUT;
}

static void lora_enter_fault_internal(const char *reason)
{
    lora_set_state(RADIO_STATE_FAULT);
    if (reason != NULL) {
        ESP_LOGE(TAG, "LoRa radio fault: %s", reason);
    }
}
