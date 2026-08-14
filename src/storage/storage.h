#pragma once

#include <stdbool.h>

typedef enum {
    STORAGE_STATE_UNINITIALIZED = 0,
    STORAGE_STATE_OK,
    STORAGE_STATE_MOUNT_FAILED,
    STORAGE_STATE_RECOVERY_ATTEMPTED,
    STORAGE_STATE_FORMAT_REQUIRED,
    STORAGE_STATE_FORMATTED,
    STORAGE_STATE_UNAVAILABLE,
} storage_state_t;

void storage_init(bool *mounted);
storage_state_t storage_get_state(void);
const char *storage_get_state_name(void);
