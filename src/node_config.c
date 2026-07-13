#include "node_config.h"

#include <string.h>

#include "esp_log.h"
#include "esp_random.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "nvs.h"
#include "mesh_protocol.h"

#define CONFIG_NAMESPACE "bems_config"
static node_config_t node_config;
static char node_id[FIELD_LEN];
static SemaphoreHandle_t config_mutex;

static void config_lock(void)
{
    if (config_mutex != NULL) {
        xSemaphoreTake(config_mutex, portMAX_DELAY);
    }
}

static void config_unlock(void)
{
    if (config_mutex != NULL) {
        xSemaphoreGive(config_mutex);
    }
}

static void ensure_mutex(void)
{
    if (config_mutex == NULL) {
        config_mutex = xSemaphoreCreateMutex();
    }
}

static void copy_field(char *destination, size_t destination_size, const char *source)
{
    size_t write_index = 0;

    if (destination_size == 0) {
        return;
    }

    while (*source != '\0' && write_index < destination_size - 1) {
        unsigned char character = (unsigned char)*source++;
        if (character >= 32 && character <= 126) {
            destination[write_index++] = (char)character;
        }
    }

    destination[write_index] = '\0';
}

static void random_hex_string(char *destination, size_t destination_size, size_t bytes)
{
    static const char digits[] = "0123456789ABCDEF";
    size_t required = bytes * 2 + 1;

    if (destination_size == 0) {
        return;
    }

    if (destination_size < required) {
        destination[0] = '\0';
        return;
    }

    for (size_t i = 0; i < bytes; i++) {
        uint8_t value = (uint8_t)esp_random();
        destination[i * 2] = digits[value >> 4];
        destination[i * 2 + 1] = digits[value & 0x0F];
    }
    destination[bytes * 2] = '\0';
}

node_config_t *node_config_get(void)
{
    return &node_config;
}

const char *node_config_get_node_id(void)
{
    return node_id;
}

const char *node_config_get_web_pin(void)
{
    return node_config.web_pin;
}

const char *node_config_get_network_key(void)
{
    return node_config.network_key;
}

const char *node_config_get_ap_password(void)
{
    return node_config.ap_password;
}

const char *node_config_get_node_role(void)
{
    return node_config.node_role;
}

const char *node_config_get_default_priority(void)
{
    return node_config.default_priority;
}

bool node_config_is_provisioned(void)
{
    return node_config.configured && node_config.web_pin[0] != '\0' && node_config.network_key[0] != '\0';
}

void node_config_set_identity(const char *identity)
{
    ensure_mutex();
    config_lock();
    copy_field(node_id, sizeof(node_id), identity);
    config_unlock();
}

void node_config_set_defaults(void)
{
    ensure_mutex();
    config_lock();
    node_config.configured = false;
    copy_field(node_config.node_id, sizeof(node_config.node_id), node_id);
    copy_field(node_config.node_name, sizeof(node_config.node_name), "Unconfigured Node");
    copy_field(node_config.node_role, sizeof(node_config.node_role), "relay-only");
    copy_field(node_config.location.sitio, sizeof(node_config.location.sitio), "");
    copy_field(node_config.location.barangay, sizeof(node_config.location.barangay), "Unknown");
    copy_field(node_config.location.municipality, sizeof(node_config.location.municipality), "");
    copy_field(node_config.default_destination, sizeof(node_config.default_destination), "BRGY001");
    copy_field(node_config.default_priority, sizeof(node_config.default_priority), "NORMAL");
    node_config.ap_password[0] = '\0';
    node_config.web_pin[0] = '\0';
    node_config.network_key[0] = '\0';
    config_unlock();
}

void node_config_load(void)
{
    nvs_handle_t handle;
    uint8_t configured = 0;
    bool migrated_location = false;
    char legacy_location[FIELD_LEN] = {0};

    ensure_mutex();
    node_config_set_defaults();

    if (nvs_open(CONFIG_NAMESPACE, NVS_READWRITE, &handle) != ESP_OK) {
        return;
    }

    nvs_get_u8(handle, "configured", &configured);
    nvs_get_str(handle, "node_id", node_config.node_id, &(size_t){sizeof(node_config.node_id)});
    nvs_get_str(handle, "node_name", node_config.node_name, &(size_t){sizeof(node_config.node_name)});
    nvs_get_str(handle, "node_role", node_config.node_role, &(size_t){sizeof(node_config.node_role)});
    nvs_get_str(handle, "default_dest", node_config.default_destination, &(size_t){sizeof(node_config.default_destination)});
    nvs_get_str(handle, "default_priority", node_config.default_priority, &(size_t){sizeof(node_config.default_priority)});
    nvs_get_str(handle, "ap_password", node_config.ap_password, &(size_t){sizeof(node_config.ap_password)});
    nvs_get_str(handle, "web_pin", node_config.web_pin, &(size_t){sizeof(node_config.web_pin)});
    nvs_get_str(handle, "network_key", node_config.network_key, &(size_t){sizeof(node_config.network_key)});
    if (nvs_get_str(handle, "sitio", node_config.location.sitio, &(size_t){sizeof(node_config.location.sitio)}) != ESP_OK) {
        copy_field(node_config.location.sitio, sizeof(node_config.location.sitio), "");
    }
    if (nvs_get_str(handle, "barangay", node_config.location.barangay, &(size_t){sizeof(node_config.location.barangay)}) != ESP_OK) {
        if (nvs_get_str(handle, "location", legacy_location, &(size_t){sizeof(legacy_location)}) == ESP_OK && legacy_location[0] != '\0') {
            location_decode(legacy_location, &node_config.location);
            migrated_location = true;
        } else {
            copy_field(node_config.location.barangay, sizeof(node_config.location.barangay), "Unknown");
        }
    }
    if (nvs_get_str(handle, "municipality", node_config.location.municipality, &(size_t){sizeof(node_config.location.municipality)}) != ESP_OK) {
        copy_field(node_config.location.municipality, sizeof(node_config.location.municipality), "");
    }

    nvs_close(handle);
    node_config.configured = configured == 1;

    if (node_config.web_pin[0] == '\0') {
        random_hex_string(node_config.web_pin, sizeof(node_config.web_pin), 4);
    }
    if (node_config.ap_password[0] == '\0') {
        random_hex_string(node_config.ap_password, sizeof(node_config.ap_password), 8);
    }
    if (node_config.network_key[0] == '\0') {
        random_hex_string(node_config.network_key, sizeof(node_config.network_key), 16);
    }
    if (node_config.node_role[0] == '\0') {
        copy_field(node_config.node_role, sizeof(node_config.node_role), "relay-only");
    }
    if (node_config.default_priority[0] == '\0') {
        copy_field(node_config.default_priority, sizeof(node_config.default_priority), "NORMAL");
    }

    if (!node_config.configured) {
        ESP_LOGW("node_config", "Provisioning defaults generated: web_pin=%s network_key=%s", node_config.web_pin, node_config.network_key);
        (void)node_config_save(&node_config);
    }

    if (migrated_location) {
        (void)node_config_save(&node_config);
    }
}

int node_config_save(const node_config_t *config)
{
    nvs_handle_t handle;
    esp_err_t result = nvs_open(CONFIG_NAMESPACE, NVS_READWRITE, &handle);

    if (result != ESP_OK) {
        return result;
    }

    result = nvs_set_u8(handle, "configured", config->configured ? 1 : 0);
    if (result == ESP_OK) result = nvs_set_str(handle, "node_id", config->node_id);
    if (result == ESP_OK) result = nvs_set_str(handle, "node_name", config->node_name);
    if (result == ESP_OK) result = nvs_set_str(handle, "node_role", config->node_role);
    if (result == ESP_OK) result = nvs_set_str(handle, "sitio", config->location.sitio);
    if (result == ESP_OK) result = nvs_set_str(handle, "barangay", config->location.barangay);
    if (result == ESP_OK) result = nvs_set_str(handle, "municipality", config->location.municipality);
    if (result == ESP_OK) result = nvs_set_str(handle, "default_dest", config->default_destination);
    if (result == ESP_OK) result = nvs_set_str(handle, "default_priority", config->default_priority);
    if (result == ESP_OK) result = nvs_set_str(handle, "ap_password", config->ap_password);
    if (result == ESP_OK) result = nvs_set_str(handle, "web_pin", config->web_pin);
    if (result == ESP_OK) result = nvs_set_str(handle, "network_key", config->network_key);
    if (result == ESP_OK) result = nvs_erase_key(handle, "location");
    if (result == ESP_OK) result = nvs_commit(handle);

    nvs_close(handle);
    return result;
}

void node_config_erase(void)
{
    nvs_handle_t handle;

    if (nvs_open(CONFIG_NAMESPACE, NVS_READWRITE, &handle) == ESP_OK) {
        nvs_erase_all(handle);
        nvs_commit(handle);
        nvs_close(handle);
    }
}
