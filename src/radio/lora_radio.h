#ifndef LORA_RADIO_H
#define LORA_RADIO_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "driver/spi_master.h"

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

bool lora_transmit(const char *packet);
bool lora_transmit_bytes(const uint8_t *packet, size_t packet_len);
typedef void (*lora_rx_callback_t)(void *parameter);
void lora_handle_rx_packet(const uint8_t *payload, size_t length, int rssi, int snr);

void lora_radio_init(void);
bool lora_radio_is_ready(void);
bool lora_channel_clear(void);

#endif
