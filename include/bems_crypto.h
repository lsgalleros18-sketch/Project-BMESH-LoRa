#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#define BEMS_CRYPTO_VERSION 1
#define BEMS_FRAME_PREFIX_0 'B'
#define BEMS_FRAME_PREFIX_1 'M'
#define BEMS_FRAME_PREFIX_2 '2'
#define BEMS_FRAME_HEADER_LEN 12
#define BEMS_NONCE_LEN 8
#define BEMS_HMAC_TAG_LEN 16

// Initializes PSA crypto library
bool crypto_init_once(void);

// Derives AES and HMAC keys from network key
bool derive_crypto_keys(uint8_t aes_key[16], uint8_t hmac_key[32]);

// AES-CTR encryption/decryption
bool aes_ctr_crypt(uint8_t *data, size_t length, const uint8_t nonce[BEMS_NONCE_LEN]);

// HMAC-SHA256 computation
bool hmac_sha256(const uint8_t *data, size_t data_len, uint8_t tag[32]);

// Encrypts a plaintext mesh packet into a BEMS frame
bool bems_encrypt_packet(const char *plain_packet, uint8_t *frame, size_t frame_size, size_t *frame_len);

// Decrypts a BEMS frame into plaintext mesh packet
bool bems_decrypt_frame(const uint8_t *frame, size_t frame_len, char *plain_packet, size_t plain_packet_size);

// Constant-time memory comparison
bool constant_time_equal(const uint8_t *left, const uint8_t *right, size_t length);

// Securely zero out sensitive data
void secure_zero(void *data, size_t length);
