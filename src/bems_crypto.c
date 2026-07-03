#include "bems_crypto.h"
#include <string.h>
#include <stddef.h>
#include <stdlib.h>
#include "esp_random.h"
#include "psa/crypto.h"

// Forward declare node_config to avoid circular dependency
extern struct {
    char network_key[32];
} node_config;

#define DEFAULT_NETWORK_KEY "CHANGEME1234567"
#define FIELD_LEN 32
#define LORA_MAX_PAYLOAD 255
#define BEMS_MAX_PLAINTEXT (LORA_MAX_PAYLOAD - BEMS_FRAME_HEADER_LEN - BEMS_HMAC_TAG_LEN)
#define MIN(a, b) ((a) < (b) ? (a) : (b))

void secure_zero(void *data, size_t length)
{
    volatile uint8_t *bytes = (volatile uint8_t *)data;

    while (length-- > 0) {
        *bytes++ = 0;
    }
}

bool constant_time_equal(const uint8_t *left, const uint8_t *right, size_t length)
{
    uint8_t diff = 0;

    for (size_t i = 0; i < length; i++) {
        diff |= left[i] ^ right[i];
    }

    return diff == 0;
}

bool crypto_init_once(void)
{
    static bool crypto_ready;

    if (!crypto_ready) {
        crypto_ready = psa_crypto_init() == PSA_SUCCESS;
    }

    return crypto_ready;
}

bool derive_crypto_keys(uint8_t aes_key[16], uint8_t hmac_key[32])
{
    uint8_t material[FIELD_LEN + 32];
    uint8_t digest[32];
    size_t key_len = strlen(node_config.network_key);
    size_t hash_length = 0;
    psa_status_t status;

    if (!crypto_init_once()) {
        return false;
    }

    if (key_len == 0) {
        key_len = strlen(DEFAULT_NETWORK_KEY);
        memcpy(material, DEFAULT_NETWORK_KEY, key_len);
    } else {
        memcpy(material, node_config.network_key, key_len);
    }

    memcpy(&material[key_len], "BMESH AES-128", 13);
    status = psa_hash_compute(PSA_ALG_SHA_256, material, key_len + 13, digest, sizeof(digest), &hash_length);
    if (status != PSA_SUCCESS || hash_length < sizeof(digest)) {
        secure_zero(material, sizeof(material));
        secure_zero(digest, sizeof(digest));
        return false;
    }
    memcpy(aes_key, digest, 16);

    memcpy(&material[key_len], "BMESH HMAC-SHA256", 17);
    status = psa_hash_compute(PSA_ALG_SHA_256, material, key_len + 17, hmac_key, 32, &hash_length);
    secure_zero(material, sizeof(material));
    secure_zero(digest, sizeof(digest));

    return status == PSA_SUCCESS && hash_length == 32;
}

bool aes_ctr_crypt(uint8_t *data, size_t length, const uint8_t nonce[BEMS_NONCE_LEN])
{
    uint8_t aes_key[16];
    uint8_t unused_hmac_key[32];
    uint8_t counter[16] = {0};
    psa_key_attributes_t attributes = PSA_KEY_ATTRIBUTES_INIT;
    psa_key_id_t key_id = 0;
    psa_cipher_operation_t operation = PSA_CIPHER_OPERATION_INIT;
    size_t output_length = 0;
    size_t finish_length = 0;
    psa_status_t status;

    if (!derive_crypto_keys(aes_key, unused_hmac_key)) {
        secure_zero(unused_hmac_key, sizeof(unused_hmac_key));
        return false;
    }
    secure_zero(unused_hmac_key, sizeof(unused_hmac_key));

    memcpy(counter, nonce, BEMS_NONCE_LEN);
    psa_set_key_type(&attributes, PSA_KEY_TYPE_AES);
    psa_set_key_bits(&attributes, 128);
    psa_set_key_usage_flags(&attributes, PSA_KEY_USAGE_ENCRYPT);
    psa_set_key_algorithm(&attributes, PSA_ALG_CTR);

    status = psa_import_key(&attributes, aes_key, sizeof(aes_key), &key_id);
    if (status == PSA_SUCCESS) {
        status = psa_cipher_encrypt_setup(&operation, key_id, PSA_ALG_CTR);
    }
    if (status == PSA_SUCCESS) {
        status = psa_cipher_set_iv(&operation, counter, sizeof(counter));
    }
    if (status == PSA_SUCCESS) {
        status = psa_cipher_update(&operation, data, length, data, length, &output_length);
    }
    if (status == PSA_SUCCESS) {
        status = psa_cipher_finish(&operation, data + output_length, length - output_length, &finish_length);
    }

    psa_cipher_abort(&operation);
    if (key_id != 0) {
        psa_destroy_key(key_id);
    }
    secure_zero(aes_key, sizeof(aes_key));
    secure_zero(counter, sizeof(counter));

    return status == PSA_SUCCESS && output_length + finish_length == length;
}

bool hmac_sha256(const uint8_t *data, size_t data_len, uint8_t tag[32])
{
    uint8_t aes_key[16];
    uint8_t hmac_key[32];
    psa_key_attributes_t attributes = PSA_KEY_ATTRIBUTES_INIT;
    psa_key_id_t key_id = 0;
    size_t tag_length = 0;
    psa_status_t status;

    if (!derive_crypto_keys(aes_key, hmac_key)) {
        return false;
    }

    psa_set_key_type(&attributes, PSA_KEY_TYPE_HMAC);
    psa_set_key_bits(&attributes, sizeof(hmac_key) * 8);
    psa_set_key_usage_flags(&attributes, PSA_KEY_USAGE_SIGN_MESSAGE);
    psa_set_key_algorithm(&attributes, PSA_ALG_HMAC(PSA_ALG_SHA_256));

    status = psa_import_key(&attributes, hmac_key, sizeof(hmac_key), &key_id);
    if (status == PSA_SUCCESS) {
        status = psa_mac_compute(key_id, PSA_ALG_HMAC(PSA_ALG_SHA_256), data, data_len, tag, 32, &tag_length);
    }

    if (key_id != 0) {
        psa_destroy_key(key_id);
    }
    secure_zero(aes_key, sizeof(aes_key));
    secure_zero(hmac_key, sizeof(hmac_key));

    return status == PSA_SUCCESS && tag_length == 32;
}

bool bems_encrypt_packet(const char *plain_packet, uint8_t *frame, size_t frame_size, size_t *frame_len)
{
    uint8_t full_tag[32];
    size_t plain_len = strlen(plain_packet);

    if (plain_len > BEMS_MAX_PLAINTEXT) {
        return false;
    }
    if (frame_size < BEMS_FRAME_HEADER_LEN + plain_len + BEMS_HMAC_TAG_LEN) {
        return false;
    }

    frame[0] = BEMS_FRAME_PREFIX_0;
    frame[1] = BEMS_FRAME_PREFIX_1;
    frame[2] = BEMS_FRAME_PREFIX_2;
    frame[3] = BEMS_CRYPTO_VERSION;

    for (size_t i = 0; i < BEMS_NONCE_LEN; i += sizeof(uint32_t)) {
        uint32_t random_value = esp_random();
        memcpy(&frame[4 + i], &random_value, MIN(sizeof(random_value), BEMS_NONCE_LEN - i));
    }

    memcpy(&frame[BEMS_FRAME_HEADER_LEN], plain_packet, plain_len);
    if (!aes_ctr_crypt(&frame[BEMS_FRAME_HEADER_LEN], plain_len, &frame[4])) {
        return false;
    }

    if (!hmac_sha256(frame, BEMS_FRAME_HEADER_LEN + plain_len, full_tag)) {
        secure_zero(full_tag, sizeof(full_tag));
        return false;
    }

    memcpy(&frame[BEMS_FRAME_HEADER_LEN + plain_len], full_tag, BEMS_HMAC_TAG_LEN);
    *frame_len = BEMS_FRAME_HEADER_LEN + plain_len + BEMS_HMAC_TAG_LEN;

    secure_zero(full_tag, sizeof(full_tag));
    return true;
}

bool bems_decrypt_frame(const uint8_t *frame, size_t frame_len, char *plain_packet, size_t plain_packet_size)
{
    uint8_t full_tag[32];
    uint8_t calculated_tag[BEMS_HMAC_TAG_LEN];
    size_t cipher_len;
    bool valid;

    if (frame_len < BEMS_FRAME_HEADER_LEN + BEMS_HMAC_TAG_LEN || plain_packet_size == 0) {
        return false;
    }
    if (frame[0] != BEMS_FRAME_PREFIX_0 || frame[1] != BEMS_FRAME_PREFIX_1 || frame[2] != BEMS_FRAME_PREFIX_2 || frame[3] != BEMS_CRYPTO_VERSION) {
        return false;
    }

    cipher_len = frame_len - BEMS_FRAME_HEADER_LEN - BEMS_HMAC_TAG_LEN;
    if (cipher_len >= plain_packet_size) {
        return false;
    }

    if (!hmac_sha256(frame, BEMS_FRAME_HEADER_LEN + cipher_len, full_tag)) {
        secure_zero(full_tag, sizeof(full_tag));
        return false;
    }

    memcpy(calculated_tag, full_tag, sizeof(calculated_tag));
    valid = constant_time_equal(calculated_tag, &frame[BEMS_FRAME_HEADER_LEN + cipher_len], BEMS_HMAC_TAG_LEN);

    if (valid) {
        memcpy(plain_packet, &frame[BEMS_FRAME_HEADER_LEN], cipher_len);
        valid = aes_ctr_crypt((uint8_t *)plain_packet, cipher_len, &frame[4]);
        plain_packet[cipher_len] = '\0';
    }

    secure_zero(full_tag, sizeof(full_tag));
    secure_zero(calculated_tag, sizeof(calculated_tag));
    return valid;
}
