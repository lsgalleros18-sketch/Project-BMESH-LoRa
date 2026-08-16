#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "messages/message_store.h"

void tx_scheduler_init(void);
bool tx_scheduler_submit(const emergency_message_t *message, int nvs_slot);
bool tx_scheduler_enqueue(const emergency_message_t *message, int nvs_slot);
bool tx_scheduler_acknowledge(uint32_t acknowledged_id, const char *ack_source);
size_t tx_scheduler_queue_depth(void);
void tx_scheduler_debug_reset_for_test(void);
