#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "driver/spi_master.h"

// Initializes LoRa radio hardware
void lora_init(void);

// Transmits a packet via LoRa
bool lora_transmit(const char *packet);

// Checks if channel is clear
bool lora_channel_clear(void);

// Sets radio mode
void lora_set_mode(uint8_t mode);

// Sets frequency
void lora_set_frequency(uint32_t frequency_hz);

// Puts radio into receive mode
void lora_receive_mode(void);

// Exposes the underlying SPI device handle for receive-path register access
spi_device_handle_t lora_get_spi(void);

// Polls for a decrypted mesh packet; returns false on timeout or if no packet is ready
bool lora_receive_plain_packet(char *packet, size_t packet_size, int *rssi, int *snr, uint32_t timeout_ms);
