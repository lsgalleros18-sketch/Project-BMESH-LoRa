#include "route_table.h"

// Never call route_table_* while holding unrelated module locks; keep the lock
// domains strictly non-nested.

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

static uint32_t route_cost_ms(const route_entry_t *entry, uint32_t now)
{
    uint32_t age_ms = now - entry->last_seen_tick_ms;
    uint32_t rssi_penalty = entry->best_rssi < 0 ? (uint32_t)(-entry->best_rssi) : 0u;
    uint32_t age_penalty = age_ms / 1000u;

    return rssi_penalty + age_penalty;
}

static size_t find_route_index(const char *destination, const char *next_hop)
{
    for (size_t i = 0; i < route_count; i++) {
        if (strcmp(route_table[i].destination, destination) == 0 &&
            strcmp(route_table[i].next_hop, next_hop) == 0) {
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

static bool route_is_valid(const char *destination, const char *next_hop, int hop_count)
{
    return destination != NULL && destination[0] != '\0' &&
           next_hop != NULL && next_hop[0] != '\0' &&
           strcmp(destination, next_hop) != 0 &&
           hop_count > 0;
}

void route_table_init(void)
{
    if (route_mutex == NULL) {
        route_mutex = xSemaphoreCreateMutex();
    }
}

void route_table_learn(const char *destination, const char *next_hop, int hop_count, int rssi)
{
    uint32_t now = now_ms();
    size_t index;
    uint32_t new_cost;

    if (!route_is_valid(destination, next_hop, hop_count)) {
        return;
    }

    route_table_init();
    route_lock();

    index = find_route_index(destination, next_hop);
    new_cost = (rssi < 0 ? (uint32_t)(-rssi) : 0u);
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
        memset(&route_table[index], 0, sizeof(route_table[index]));
        copy_field(route_table[index].destination, sizeof(route_table[index].destination), destination);
        copy_field(route_table[index].next_hop, sizeof(route_table[index].next_hop), next_hop);
    }

    refresh_stale_state(&route_table[index], now);
    if (route_table[index].stale ||
        rssi > route_table[index].best_rssi ||
        new_cost < route_cost_ms(&route_table[index], now) ||
        route_table[index].hop_count == 0) {
        route_table[index].hop_count = hop_count;
        route_table[index].best_rssi = rssi;
        route_table[index].cost = new_cost;
    }

    route_table[index].last_seen_tick_ms = now;
    refresh_stale_state(&route_table[index], now);

    route_unlock();
}

bool route_table_get_best(const char *destination, route_entry_t *out)
{
    bool found = false;
    uint32_t now;
    uint32_t best_cost = UINT32_MAX;
    route_entry_t best = {0};

    if (destination == NULL || destination[0] == '\0' || out == NULL) {
        return false;
    }

    route_table_init();
    now = now_ms();
    route_lock();
    for (size_t i = 0; i < route_count; i++) {
        route_entry_t candidate = route_table[i];

        if (strcmp(candidate.destination, destination) != 0) {
            continue;
        }

        refresh_stale_state(&candidate, now);
        if (candidate.stale || candidate.hop_count <= 0 || strcmp(candidate.next_hop, destination) == 0) {
            continue;
        }

        if (!found ||
            candidate.cost < best_cost ||
            (candidate.cost == best_cost && candidate.hop_count < best.hop_count) ||
            (candidate.cost == best_cost && candidate.hop_count == best.hop_count && candidate.best_rssi > best.best_rssi) ||
            (candidate.cost == best_cost && candidate.hop_count == best.hop_count && candidate.best_rssi == best.best_rssi &&
             strcmp(candidate.next_hop, best.next_hop) < 0)) {
            best = candidate;
            best_cost = candidate.cost;
            found = true;
        }
    }
    route_unlock();

    if (found) {
        *out = best;
    }
    return found;
}

bool route_table_lookup(const char *destination, route_entry_t *out)
{
    return route_table_get_best(destination, out);
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

void route_table_debug_set_last_seen_for_test(const char *destination, const char *next_hop, uint32_t last_seen_tick_ms)
{
    size_t index;
    uint32_t now = now_ms();

    route_table_init();
    route_lock();
    index = find_route_index(destination, next_hop);
    if (index != SIZE_MAX) {
        route_table[index].last_seen_tick_ms = last_seen_tick_ms;
        refresh_stale_state(&route_table[index], now);
    }
    route_unlock();
}

void route_table_debug_reset_for_test(void)
{
    route_table_init();
    route_lock();
    memset(route_table, 0, sizeof(route_table));
    route_count = 0;
    route_unlock();
}
