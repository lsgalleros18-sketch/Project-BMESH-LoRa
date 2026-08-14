#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

#include "bems_common.h"

#define MAX_ROUTE_ENTRIES 32
#define ROUTE_STALE_MS 300000

typedef struct {
    char destination[FIELD_LEN];
    char next_hop[FIELD_LEN];
    int hop_count;
    int best_rssi;
    uint32_t cost;
    uint32_t last_seen_tick_ms;
    bool stale;
} route_entry_t;

void route_table_init(void);
void route_table_learn(const char *destination, const char *next_hop, int hop_count, int rssi);
bool route_table_get_best(const char *destination, route_entry_t *out);
bool route_table_lookup(const char *destination, route_entry_t *out);
size_t route_table_get_snapshot(route_entry_t *out, size_t out_capacity);
void route_table_debug_set_last_seen_for_test(const char *destination, const char *next_hop, uint32_t last_seen_tick_ms);
void route_table_debug_reset_for_test(void);
