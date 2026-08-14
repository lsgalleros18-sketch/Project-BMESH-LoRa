#pragma once

#include <stdbool.h>
#include <stdint.h>

void mesh_sequence_init(void);
uint32_t mesh_sequence_next(void);
uint32_t mesh_sequence_peek(void);
void mesh_sequence_update(uint32_t sequence);
void mesh_sequence_save(void);
