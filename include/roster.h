#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

#include "bems_common.h"

#define MAX_ROSTER_ENTRIES 32
#define ROSTER_STALE_MS 300000

typedef struct {
    char node_id[FIELD_LEN];
    location_info_t location;
    uint32_t last_seen_epoch;
    uint32_t last_seen_tick_ms;
    bool online;
    bool learned_passively;
} roster_entry_t;

void roster_init(void);
void roster_touch(const char *node_id, const location_info_t *location, uint32_t epoch_seconds);
void roster_mark_active(const char *node_id);
bool roster_is_stale(const char *node_id);
bool roster_is_stale_at(const char *node_id, uint32_t now_ms);
size_t roster_get_snapshot(roster_entry_t *out, size_t out_capacity);
