#include "mesh/forward_queue.h"

#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

static QueueHandle_t forward_queues[3];

void forward_queue_init(void)
{
    for (size_t i = 0; i < 3; i++) {
        if (forward_queues[i] == NULL) {
            forward_queues[i] = xQueueCreate(FORWARD_QUEUE_DEPTH, sizeof(forward_job_t));
        }
    }
}

static QueueHandle_t queue_for_priority(tx_priority_t priority)
{
    switch (priority) {
    case TX_PRIORITY_HIGH:
        return forward_queues[0];
    case TX_PRIORITY_LOW:
        return forward_queues[2];
    default:
        return forward_queues[1];
    }
}

bool forward_queue_enqueue(const forward_job_t *job, tx_priority_t priority)
{
    if (job == NULL) {
        return false;
    }

    forward_queue_init();
    QueueHandle_t queue = queue_for_priority(priority);
    if (queue == NULL) {
        return false;
    }

    return xQueueSend(queue, job, 0) == pdTRUE;
}

bool forward_queue_receive(forward_job_t *job, TickType_t timeout_ticks)
{
    forward_queue_init();
    if (job == NULL) {
        return false;
    }

    if (forward_queues[0] != NULL && xQueueReceive(forward_queues[0], job, 0) == pdTRUE) {
        return true;
    }

    static uint8_t normal_budget = 2;
    static uint8_t low_budget = 1;

    if (forward_queues[1] != NULL && normal_budget > 0 && xQueueReceive(forward_queues[1], job, 0) == pdTRUE) {
        normal_budget--;
        low_budget = 1;
        return true;
    }

    if (forward_queues[2] != NULL && low_budget > 0 && xQueueReceive(forward_queues[2], job, 0) == pdTRUE) {
        low_budget--;
        normal_budget = 2;
        return true;
    }

    if (forward_queues[1] != NULL && xQueueReceive(forward_queues[1], job, timeout_ticks) == pdTRUE) {
        normal_budget = 1;
        low_budget = 1;
        return true;
    }

    if (forward_queues[2] != NULL && xQueueReceive(forward_queues[2], job, timeout_ticks) == pdTRUE) {
        low_budget = 1;
        return true;
    }

    return false;
}
