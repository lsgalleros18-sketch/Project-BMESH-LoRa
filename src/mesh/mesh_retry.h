#pragma once

#include <stdint.h>

void mesh_retry_init(void);
void mesh_retry_track(uint32_t id, const char *source, const char *destination, const char *priority);
