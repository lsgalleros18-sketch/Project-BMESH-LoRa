#include "mesh/mesh_sequence.h"

#include "esp_err.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "nvs.h"

static const char *TAG = "mesh_sequence";
static SemaphoreHandle_t sequence_mutex;
static uint32_t sequence_counter;
static bool sequence_loaded;

static void lock(void)
{
    if (sequence_mutex == NULL) {
        sequence_mutex = xSemaphoreCreateMutex();
    }
    if (sequence_mutex != NULL) {
        xSemaphoreTake(sequence_mutex, portMAX_DELAY);
    }
}

static void unlock(void)
{
    if (sequence_mutex != NULL) {
        xSemaphoreGive(sequence_mutex);
    }
}

static void load_impl(void)
{
    nvs_handle_t handle;
    uint32_t stored = 0;

    if (sequence_loaded) {
        return;
    }

    if (nvs_open("bems_config", NVS_READONLY, &handle) != ESP_OK) {
        ESP_LOGI(TAG, "No saved packet counter; starting at 0");
        sequence_loaded = true;
        return;
    }

    if (nvs_get_u32(handle, "packet_ctr", &stored) == ESP_OK) {
        sequence_counter = stored;
        ESP_LOGI(TAG, "Packet counter restored: %lu", (unsigned long)sequence_counter);
    } else {
        ESP_LOGI(TAG, "No saved packet counter; starting at 0");
    }

    nvs_close(handle);
    sequence_loaded = true;
}

void mesh_sequence_init(void)
{
    lock();
    load_impl();
    unlock();
}

uint32_t mesh_sequence_next(void)
{
    uint32_t next;

    lock();
    load_impl();
    next = ++sequence_counter;
    unlock();
    return next;
}

uint32_t mesh_sequence_peek(void)
{
    uint32_t current;

    lock();
    load_impl();
    current = sequence_counter;
    unlock();
    return current;
}

void mesh_sequence_update(uint32_t sequence)
{
    lock();
    load_impl();
    if (sequence > sequence_counter) {
        sequence_counter = sequence;
    }
    unlock();
}

void mesh_sequence_save(void)
{
    nvs_handle_t handle;
    esp_err_t result;

    lock();
    load_impl();
    result = nvs_open("bems_config", NVS_READWRITE, &handle);
    if (result == ESP_OK) {
        result = nvs_set_u32(handle, "packet_ctr", sequence_counter);
        if (result == ESP_OK) {
            result = nvs_commit(handle);
        }
        nvs_close(handle);
    }
    unlock();

    if (result != ESP_OK) {
        ESP_LOGW(TAG, "Failed to save packet counter: %s", esp_err_to_name(result));
    }
}
