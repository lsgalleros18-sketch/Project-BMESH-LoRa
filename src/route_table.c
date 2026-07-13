#include "route_table.h"

// Never call route_table_* while holding main.c's data_mutex, and never call data_lock()
// from inside route_table_*; keep the lock domains strictly non-nested.

#include <string.h>

#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static route_entry_t route_table[MAX_ROUTE_ENTRIES];
static size_t route_count;
static SemaphoreHandle_t route_mutex;

static void route_lock(void)
{
    if (route_mutex != NULL) {
        xSemaphoreTake(route_mutex, portMAX_DELAY);
    }
}

static void route_unlock(void)
{
    if (route_mutex != NULL) {
        xSemaphoreGive(route_mutex);
    }
}

static void copy_field(char *destination, size_t destination_size, const char *source)
{
    size_t write_index = 0;

    if (destination_size == 0) {
        return;
    }

    while (source != NULL && *source != '\0' && write_index < destination_size - 1) {
        unsigned char character = (unsigned char)*source++;
        if (character >= 32 && character <= 126) {
            destination[write_index++] = (char)character;
        }
    }

    destination[write_index] = '\0';
}

static uint32_t now_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000ULL);
}

static size_t find_route_index(const char *node_id)
{
    for (size_t i = 0; i < route_count; i++) {
        if (strcmp(route_table[i].node_id, node_id) == 0) {
            return i;
        }
    }

    return SIZE_MAX;
}

static bool route_is_stale_locked(const route_entry_t *entry, uint32_t now)
{
    return (now - entry->last_seen_tick_ms) >= ROUTE_STALE_MS;
}

static void refresh_stale_state(route_entry_t *entry, uint32_t now)
{
    entry->stale = route_is_stale_locked(entry, now);
}

void route_table_init(void)
{
    if (route_mutex == NULL) {
        route_mutex = xSemaphoreCreateMutex();
    }
}

void route_table_learn(const char *node_id, int hop_distance_from_origin)
{
    uint32_t now = now_ms();
    size_t index;

    if (node_id == NULL || node_id[0] == '\0') {
        return;
    }

    route_table_init();
    route_lock();

    index = find_route_index(node_id);
    if (index == SIZE_MAX) {
        if (route_count < MAX_ROUTE_ENTRIES) {
            index = route_count++;
        } else {
            size_t oldest_index = 0;
            for (size_t i = 1; i < route_count; i++) {
                if ((now - route_table[i].last_seen_tick_ms) > (now - route_table[oldest_index].last_seen_tick_ms)) {
                    oldest_index = i;
                }
            }
            index = oldest_index;
        }
        copy_field(route_table[index].node_id, sizeof(route_table[index].node_id), node_id);
        route_table[index].best_hop_distance = hop_distance_from_origin;
    } else {
        refresh_stale_state(&route_table[index], now);
        if (route_table[index].stale || hop_distance_from_origin <= route_table[index].best_hop_distance) {
            route_table[index].best_hop_distance = hop_distance_from_origin;
        }
    }

    route_table[index].last_seen_tick_ms = now;
    refresh_stale_state(&route_table[index], now);

    route_unlock();
}

bool route_table_lookup(const char *node_id, route_entry_t *out)
{
    bool found = false;
    size_t index;
    uint32_t now;

    if (node_id == NULL || node_id[0] == '\0' || out == NULL) {
        return false;
    }

    route_table_init();
    route_lock();
    index = find_route_index(node_id);
    if (index != SIZE_MAX) {
        now = now_ms();
        *out = route_table[index];
        refresh_stale_state(out, now);
        found = true;
    }
    route_unlock();

    return found;
}

size_t route_table_get_snapshot(route_entry_t *out, size_t out_capacity)
{
    size_t count;
    uint32_t now;

    if (out == NULL || out_capacity == 0) {
        return 0;
    }

    route_table_init();
    now = now_ms();
    route_lock();
    count = route_count < out_capacity ? route_count : out_capacity;
    for (size_t i = 0; i < count; i++) {
        refresh_stale_state(&route_table[i], now);
        out[i] = route_table[i];
    }
    route_unlock();

    return count;
}
