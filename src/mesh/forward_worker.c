#include "mesh/forward_worker.h"

#include <string.h>

#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "app/app_runtime.h"
#include "mesh/forward_queue.h"
#include "mesh_protocol.h"
#include "radio/lora_radio.h"
#include "utils/string_utils.h"

static const char *TAG = "forward_worker";
static TaskHandle_t forward_worker_task_handle;

static void forward_worker_task(void *parameter)
{
    forward_job_t job;

    (void)parameter;
    while (true) {
        if (!forward_queue_receive(&job, portMAX_DELAY)) {
            continue;
        }

        uint32_t delay_ms = 100 + (esp_random() % 500);
        vTaskDelay(pdMS_TO_TICKS(delay_ms));

        if (packet_seen(job.packet.source, job.packet.id)) {
            ESP_LOGI(TAG, "Suppressed duplicate forward for %s/%lu after %u ms", job.packet.source, (unsigned long)job.packet.id, (unsigned int)delay_ms);
            continue;
        }

        if (!app_runtime_should_forward_packet(&job.packet, job.route_known ? &job.route : NULL, job.local_node_id)) {
            continue;
        }

        if (!mesh_packet_consume_hop(&job.packet)) {
            ESP_LOGI(TAG, "Dropped packet %s/%lu because hop budget was exhausted before forwarding",
                     job.packet.source,
                     (unsigned long)job.packet.id);
            continue;
        }

        if (job.route_known) {
            ESP_LOGI(TAG, "Route-selected forward for %s -> %s via %s (hop=%d rssi=%d age=%lu ms)",
                     job.packet.source,
                     job.packet.destination,
                     job.route.next_hop,
                     job.route.hop_count,
                     job.route.best_rssi,
                     (unsigned long)(esp_timer_get_time() / 1000ULL - job.route.last_seen_tick_ms));
        } else {
            ESP_LOGI(TAG, "Flood fallback forward for %s -> %s", job.packet.source, job.packet.destination);
        }

        copy_field(job.packet.relay, sizeof(job.packet.relay), job.packet.source);
        if (job.route_known) {
            copy_field(job.packet.next_hop, sizeof(job.packet.next_hop), job.route.next_hop);
        } else {
            job.packet.next_hop[0] = '\0';
        }

        uint8_t forward_packet[PACKET_LEN];
        size_t forward_packet_len = 0;
        if (!build_forward_packet_v2(&job.packet, forward_packet, sizeof(forward_packet), &forward_packet_len)) {
            ESP_LOGW(TAG, "Failed to build V2 forward packet for %s/%lu", job.packet.source, (unsigned long)job.packet.id);
            continue;
        }

        ESP_LOGI(TAG, "Forwarding packet %s/%lu after %u ms delay", job.packet.source, (unsigned long)job.packet.id, (unsigned int)delay_ms);
        (void)lora_transmit_bytes(forward_packet, forward_packet_len);
    }
}

void forward_worker_init(void)
{
    forward_queue_init();
    if (forward_worker_task_handle == NULL) {
        xTaskCreate(forward_worker_task, "forward_worker", 4096, NULL, 5, &forward_worker_task_handle);
    }
}
