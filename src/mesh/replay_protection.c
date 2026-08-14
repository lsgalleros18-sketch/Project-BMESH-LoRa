#include "mesh/replay_protection.h"

#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include "bems_common.h"

static void copy_field(char *destination, size_t destination_size, const char *source)
{
    size_t write_index = 0;

    if (destination_size == 0) {
        return;
    }

    while (*source != '\0' && write_index < destination_size - 1) {
        unsigned char character = (unsigned char)*source++;
        if (character >= 32 && character <= 126) {
            destination[write_index++] = (char)character;
        }
    }

    destination[write_index] = '\0';
}

typedef struct {
    bool active;
    char source[FIELD_LEN];
    uint32_t highest_sequence;
    uint32_t window_bits;
    uint32_t last_activity_ticks;
} replay_source_state_t;

static SemaphoreHandle_t replay_mutex;
static replay_source_state_t replay_sources[REPLAY_SOURCE_TABLE_SIZE];

static void ensure_mutex(void)
{
    if (replay_mutex == NULL) {
        replay_mutex = xSemaphoreCreateMutex();
    }
}

static void lock(void)
{
    if (replay_mutex != NULL) {
        xSemaphoreTake(replay_mutex, portMAX_DELAY);
    }
}

static void unlock(void)
{
    if (replay_mutex != NULL) {
        xSemaphoreGive(replay_mutex);
    }
}

static void reset_entry(replay_source_state_t *entry)
{
    memset(entry, 0, sizeof(*entry));
}

static bool sequence_is_newer(uint32_t a, uint32_t b)
{
    return a != b && (uint32_t)(a - b) < 0x80000000u;
}

static bool sequence_is_older(uint32_t a, uint32_t b)
{
    return sequence_is_newer(b, a);
}

static int find_source_index(const char *source)
{
    for (size_t i = 0; i < REPLAY_SOURCE_TABLE_SIZE; i++) {
        if (replay_sources[i].active && strcmp(replay_sources[i].source, source) == 0) {
            return (int)i;
        }
    }

    return -1;
}

static int allocate_source_index(uint32_t now_ticks)
{
    int free_index = -1;
    int oldest_index = -1;

    for (size_t i = 0; i < REPLAY_SOURCE_TABLE_SIZE; i++) {
        if (!replay_sources[i].active) {
            return (int)i;
        }
        if (free_index < 0) {
            free_index = (int)i;
        }
        if (oldest_index < 0 || (uint32_t)(now_ticks - replay_sources[i].last_activity_ticks) > (uint32_t)(now_ticks - replay_sources[oldest_index].last_activity_ticks)) {
            oldest_index = (int)i;
        }
    }

    return oldest_index >= 0 ? oldest_index : free_index;
}

static bool accept_new_sequence(replay_source_state_t *state, uint32_t sequence, uint32_t now_ticks)
{
    uint32_t delta = sequence - state->highest_sequence;

    if (delta == 0) {
        return false;
    }

    if (delta < 0x80000000u) {
        if (delta >= REPLAY_WINDOW_SIZE) {
            state->highest_sequence = sequence;
            state->window_bits = 1u;
        } else {
            state->window_bits = (state->window_bits << delta) | 1u;
            state->highest_sequence = sequence;
        }
        state->last_activity_ticks = now_ticks;
        return true;
    }

    return false;
}

static bool accept_older_sequence(replay_source_state_t *state, uint32_t sequence, uint32_t now_ticks)
{
    uint32_t diff = state->highest_sequence - sequence;
    uint32_t mask;

    if (diff >= REPLAY_WINDOW_SIZE) {
        return false;
    }

    mask = 1u << diff;
    if ((state->window_bits & mask) != 0u) {
        return false;
    }

    state->window_bits |= mask;
    state->last_activity_ticks = now_ticks;
    return true;
}

bool replay_protection_accept(const char *source, uint32_t sequence, uint32_t now_ticks)
{
    int index;
    replay_source_state_t *state;
    bool accepted = false;

    if (source == NULL || source[0] == '\0') {
        return false;
    }

    ensure_mutex();
    lock();

    index = find_source_index(source);
    if (index < 0) {
        index = allocate_source_index(now_ticks);
        if (index >= 0) {
            state = &replay_sources[index];
            reset_entry(state);
            state->active = true;
            copy_field(state->source, sizeof(state->source), source);
            state->highest_sequence = sequence;
            state->window_bits = 1u;
            state->last_activity_ticks = now_ticks;
            accepted = true;
        }
        unlock();
        return accepted;
    }

    state = &replay_sources[index];
    if (sequence_is_newer(sequence, state->highest_sequence)) {
        accepted = accept_new_sequence(state, sequence, now_ticks);
    } else if (sequence_is_older(sequence, state->highest_sequence)) {
        accepted = accept_older_sequence(state, sequence, now_ticks);
    }

    if (accepted) {
        state->last_activity_ticks = now_ticks;
    }

    unlock();
    return accepted;
}

void replay_protection_reset(void)
{
    ensure_mutex();
    lock();
    for (size_t i = 0; i < REPLAY_SOURCE_TABLE_SIZE; i++) {
        reset_entry(&replay_sources[i]);
    }
    unlock();
}
