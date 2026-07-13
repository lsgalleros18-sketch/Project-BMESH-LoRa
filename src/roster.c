#include "roster.h"

// Never call roster_* while holding main.c's data_mutex, and never call data_lock()
// from inside roster_*; keep the lock domains strictly non-nested.

#include <string.h>

#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static roster_entry_t roster[MAX_ROSTER_ENTRIES];
static size_t roster_count;
static SemaphoreHandle_t roster_mutex;

static void roster_lock(void)
{
    if (roster_mutex != NULL) {
        xSemaphoreTake(roster_mutex, portMAX_DELAY);
    }
}

static void roster_unlock(void)
{
    if (roster_mutex != NULL) {
        xSemaphoreGive(roster_mutex);
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

static size_t find_roster_index(const char *node_id)
{
    for (size_t i = 0; i < roster_count; i++) {
        if (strcmp(roster[i].node_id, node_id) == 0) {
            return i;
        }
    }

    return SIZE_MAX;
}

static void refresh_online_state(roster_entry_t *entry, uint32_t now)
{
    entry->online = (now - entry->last_seen_tick_ms) < ROSTER_STALE_MS;
}

static bool roster_is_stale_locked(const char *node_id, uint32_t now)
{
    size_t index = find_roster_index(node_id);

    if (index == SIZE_MAX) {
        return true;
    }

    return (now - roster[index].last_seen_tick_ms) >= ROSTER_STALE_MS;
}

void roster_init(void)
{
    if (roster_mutex == NULL) {
        roster_mutex = xSemaphoreCreateMutex();
    }
}

void roster_touch(const char *node_id, const location_info_t *location, uint32_t epoch_seconds)
{
    uint32_t now = now_ms();
    size_t index;

    if (node_id == NULL || node_id[0] == '\0') {
        return;
    }

    roster_init();
    roster_lock();

    index = find_roster_index(node_id);
    if (index == SIZE_MAX) {
        if (roster_count < MAX_ROSTER_ENTRIES) {
            index = roster_count++;
        } else {
            size_t oldest_index = 0;
            for (size_t i = 1; i < roster_count; i++) {
                if ((now - roster[i].last_seen_tick_ms) > (now - roster[oldest_index].last_seen_tick_ms)) {
                    oldest_index = i;
                }
            }
            index = oldest_index;
        }
    }

    copy_field(roster[index].node_id, sizeof(roster[index].node_id), node_id);
    if (location != NULL) {
        roster[index].location = *location;
    } else {
        memset(&roster[index].location, 0, sizeof(roster[index].location));
    }
    roster[index].last_seen_epoch = epoch_seconds;
    roster[index].last_seen_tick_ms = now;
    refresh_online_state(&roster[index], now);

    roster_unlock();
}

bool roster_is_stale_at(const char *node_id, uint32_t now)
{
    bool stale;

    if (node_id == NULL || node_id[0] == '\0') {
        return true;
    }

    roster_init();
    roster_lock();
    stale = roster_is_stale_locked(node_id, now);
    roster_unlock();

    return stale;
}

bool roster_is_stale(const char *node_id)
{
    return roster_is_stale_at(node_id, now_ms());
}

size_t roster_get_snapshot(roster_entry_t *out, size_t out_capacity)
{
    size_t count;
    uint32_t now;

    if (out == NULL || out_capacity == 0) {
        return 0;
    }

    roster_init();
    now = now_ms();
    roster_lock();
    count = roster_count < out_capacity ? roster_count : out_capacity;
    for (size_t i = 0; i < count; i++) {
        roster[i].online = !roster_is_stale_locked(roster[i].node_id, now);
        out[i] = roster[i];
    }
    roster_unlock();

    return count;
}
