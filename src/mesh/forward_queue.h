#pragma once

#include <stdbool.h>

#include "freertos/FreeRTOS.h"
#include "mesh_protocol.h"
#include "route_table.h"

#define FORWARD_QUEUE_DEPTH 8

typedef struct {
    mesh_packet_t packet;
    int rssi;
    int snr;
    route_entry_t route;
    bool route_known;
    char local_node_id[FIELD_LEN];
} forward_job_t;

typedef enum {
    TX_PRIORITY_HIGH = 0,
    TX_PRIORITY_NORMAL = 1,
    TX_PRIORITY_LOW = 2,
} tx_priority_t;

void forward_queue_init(void);
bool forward_queue_enqueue(const forward_job_t *job, tx_priority_t priority);
bool forward_queue_receive(forward_job_t *job, TickType_t timeout_ticks);
