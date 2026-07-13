#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

#include "bems_common.h"

#define MAX_ROUTE_ENTRIES 32
#define ROUTE_STALE_MS 300000

typedef struct {
    char node_id[FIELD_LEN];
    int best_hop_distance;
    uint32_t last_seen_tick_ms;
    bool stale;
} route_entry_t;

void route_table_init(void);
void route_table_learn(const char *node_id, int hop_distance_from_origin);
bool route_table_lookup(const char *node_id, route_entry_t *out);
size_t route_table_get_snapshot(route_entry_t *out, size_t out_capacity);
