#include "storage/storage.h"

#include "esp_littlefs.h"
#include "esp_log.h"

#define LITTLEFS_BASE_PATH "/littlefs"

static const char *TAG = "storage";

void storage_init(bool *mounted)
{
    esp_vfs_littlefs_conf_t config = {
        .base_path = LITTLEFS_BASE_PATH,
        .partition_label = "storage",
        .format_if_mount_failed = true,
        .dont_mount = false,
    };
    size_t total_bytes = 0;
    size_t used_bytes = 0;
    esp_err_t result = esp_vfs_littlefs_register(&config);

    if (result != ESP_OK) {
        ESP_LOGE(TAG, "LittleFS mount failed for partition '%s': %s", config.partition_label, esp_err_to_name(result));
        if (mounted != NULL) {
            *mounted = false;
        }
        return;
    }

    if (mounted != NULL) {
        *mounted = true;
    }

    result = esp_littlefs_info(config.partition_label, &total_bytes, &used_bytes);
    if (result == ESP_OK) {
        ESP_LOGI(TAG, "LittleFS mounted at %s: %u/%u bytes used", config.base_path, (unsigned int)used_bytes, (unsigned int)total_bytes);
    } else {
        ESP_LOGW(TAG, "LittleFS mounted, but space query failed: %s", esp_err_to_name(result));
    }
}
