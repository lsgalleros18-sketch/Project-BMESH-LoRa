#include "storage/storage.h"

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "esp_littlefs.h"
#include "esp_log.h"
#include "esp_partition.h"
#include "nvs.h"

#include "utils/string_utils.h"

#define LITTLEFS_BASE_PATH "/littlefs"

static const char *TAG = "storage";
static storage_state_t storage_state = STORAGE_STATE_UNINITIALIZED;
static const char *MESSAGE_NAMESPACE = "bems_config";

static const char *state_name(storage_state_t state)
{
    switch (state) {
    case STORAGE_STATE_OK: return "FS_OK";
    case STORAGE_STATE_MOUNT_FAILED: return "FS_MOUNT_FAILED";
    case STORAGE_STATE_RECOVERY_ATTEMPTED: return "FS_RECOVERY_ATTEMPTED";
    case STORAGE_STATE_FORMAT_REQUIRED: return "FS_FORMAT_REQUIRED";
    case STORAGE_STATE_FORMATTED: return "FS_FORMATTED";
    case STORAGE_STATE_UNAVAILABLE: return "FS_UNAVAILABLE";
    default: return "FS_UNINITIALIZED";
    }
}

storage_state_t storage_get_state(void)
{
    return storage_state;
}

const char *storage_get_state_name(void)
{
    return state_name(storage_state);
}

static void report_failure(const char *reason, esp_err_t err)
{
    ESP_LOGE(TAG, "%s: %s", reason, esp_err_to_name(err));
}

static esp_err_t try_mount(bool format_if_mount_failed)
{
    esp_vfs_littlefs_conf_t config = {
        .base_path = LITTLEFS_BASE_PATH,
        .partition_label = "storage",
        .format_if_mount_failed = format_if_mount_failed,
        .dont_mount = false,
    };

    return esp_vfs_littlefs_register(&config);
}

void storage_init(bool *mounted)
{
    size_t total_bytes = 0;
    size_t used_bytes = 0;
    esp_err_t result;

    storage_state = STORAGE_STATE_MOUNT_FAILED;
    result = try_mount(false);
    if (result != ESP_OK) {
        report_failure("LittleFS mount failed", result);
        storage_state = STORAGE_STATE_RECOVERY_ATTEMPTED;
        if (esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_LITTLEFS, "storage") == NULL) {
            storage_state = STORAGE_STATE_UNAVAILABLE;
            report_failure("LittleFS partition not found", ESP_ERR_NOT_FOUND);
        } else {
            storage_state = STORAGE_STATE_FORMAT_REQUIRED;
            ESP_LOGW(TAG, "LittleFS recovery requires explicit format; leaving storage unavailable");
        }
        if (mounted != NULL) {
            *mounted = false;
        }
        return;
    }

    storage_state = STORAGE_STATE_OK;
    if (mounted != NULL) {
        *mounted = true;
    }

    result = esp_littlefs_info("storage", &total_bytes, &used_bytes);
    if (result == ESP_OK) {
        ESP_LOGI(TAG, "LittleFS mounted at %s: %u/%u bytes used", LITTLEFS_BASE_PATH, (unsigned int)used_bytes, (unsigned int)total_bytes);
    } else {
        ESP_LOGW(TAG, "LittleFS mounted, but space query failed: %s", esp_err_to_name(result));
    }
}

void storage_message_save(const emergency_message_t *message, int slot)
{
    nvs_handle_t handle;
    char key[16];
    esp_err_t result;

    if (message == NULL || slot < 0 || slot >= MAX_MESSAGES) {
        return;
    }

    result = nvs_open(MESSAGE_NAMESPACE, NVS_READWRITE, &handle);
    if (result != ESP_OK) {
        ESP_LOGW(TAG, "Failed to open NVS for message save: %s", esp_err_to_name(result));
        return;
    }

    snprintf(key, sizeof(key), "msg_%d", slot);
    result = nvs_set_blob(handle, key, (const void *)message, sizeof(*message));
    if (result == ESP_OK) {
        result = nvs_commit(handle);
    }
    if (result != ESP_OK) {
        ESP_LOGW(TAG, "Failed to save message %d to NVS: %s", slot, esp_err_to_name(result));
    }
    nvs_close(handle);
}

static bool message_is_restorable(const emergency_message_t *message)
{
    if (message == NULL || message->id == 0) {
        return false;
    }
    return strcmp(message->direction, "TX") == 0 || strcmp(message->direction, "RX") == 0;
}

void storage_message_load_messages(const char *node_id, emergency_message_t *messages, size_t *count, size_t max_messages)
{
    nvs_handle_t handle;
    char key[16];
    esp_err_t result;
    size_t blob_size;
    emergency_message_t loaded_message;
    size_t loaded_count = 0;

    if (messages == NULL || count == NULL || max_messages == 0) {
        return;
    }

    *count = 0;
    result = nvs_open(MESSAGE_NAMESPACE, NVS_READONLY, &handle);
    if (result != ESP_OK) {
        ESP_LOGI(TAG, "No saved messages in NVS");
        return;
    }

    for (int i = 0; i < MAX_MESSAGES && loaded_count < max_messages; i++) {
        snprintf(key, sizeof(key), "msg_%d", i);
        blob_size = sizeof(loaded_message);
        result = nvs_get_blob(handle, key, &loaded_message, &blob_size);
        if (result != ESP_OK || blob_size != sizeof(loaded_message)) {
            continue;
        }
        if (!message_is_restorable(&loaded_message)) {
            continue;
        }
        if (loaded_message.direction[0] == '\0' || loaded_message.source[0] == '\0' || loaded_message.destination[0] == '\0') {
            continue;
        }
        if ((strcmp(loaded_message.source, node_id) != 0) && (strcmp(loaded_message.destination, node_id) != 0)) {
            continue;
        }
        memcpy(&messages[loaded_count++], &loaded_message, sizeof(loaded_message));
    }

    *count = loaded_count;
    ESP_LOGI(TAG, "Loaded %zu messages from NVS", loaded_count);
    nvs_close(handle);
}
