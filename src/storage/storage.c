#include "storage/storage.h"

#include <stdbool.h>

#include "esp_littlefs.h"
#include "esp_log.h"
#include "esp_partition.h"

#define LITTLEFS_BASE_PATH "/littlefs"

static const char *TAG = "storage";
static storage_state_t storage_state = STORAGE_STATE_UNINITIALIZED;

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
