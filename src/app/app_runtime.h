#pragma once

#include <stdbool.h>

#include "mesh_protocol.h"
#include "route_table.h"

void app_runtime_start(void);
bool app_runtime_should_forward_packet(const mesh_packet_t *packet, const route_entry_t *route, const char *local_node_id);
