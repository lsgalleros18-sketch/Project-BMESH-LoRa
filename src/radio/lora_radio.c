#include "radio/lora_radio.h"

#include <string.h>

#include "esp_err.h"
#include "driver/spi_master.h"

static spi_device_handle_t lora_spi;

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
