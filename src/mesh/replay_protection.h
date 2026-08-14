#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define REPLAY_WINDOW_SIZE 32
#define REPLAY_SOURCE_TABLE_SIZE 16

bool replay_protection_accept(const char *source, uint32_t sequence, uint32_t now_ticks);
void replay_protection_reset(void);

