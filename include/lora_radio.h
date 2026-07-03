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

// Gets SPI device handle (for internal use)
extern spi_device_handle_t lora_spi;
