#include "http/http_messages.h"

#include <stddef.h>

#include "esp_log.h"

#include "json/json_writer.h"
#include "messages/message_store.h"

static const char *TAG = "mesh_portal";

esp_err_t http_messages_handler(httpd_req_t *request)
{
    size_t snapshot_count = 0;
    const http_messages_context_t *context = request->user_ctx;
    esp_err_t session_result = context->require_session(request);
    esp_err_t chunk_result;
    const emergency_message_t *snapshot;

    if (session_result != ESP_OK) {
        ESP_LOGW(TAG, "/api/messages rejected: %s", esp_err_to_name(session_result));
        return session_result;
    }

    httpd_resp_set_type(request, "application/json");
    ESP_LOGI(TAG, "messages_handler: sending opening '['");
    chunk_result = httpd_resp_send_chunk(request, "[", 1);
    ESP_LOGI(TAG, "messages_handler: opening '[' result=%s", esp_err_to_name(chunk_result));
    if (chunk_result != ESP_OK) {
        return chunk_result;
    }
    snapshot = message_store_snapshot(&snapshot_count);
    ESP_LOGI(TAG, "messages_handler: message_count=%u", (unsigned int)snapshot_count);
    ESP_LOGI(TAG, "messages_handler: snapshot_count=%u after copy", (unsigned int)snapshot_count);
    for (size_t i = 0; i < snapshot_count; i++) {
        ESP_LOGI(TAG, "messages_handler: serializing snapshot[%u] id=%lu first=%d",
                 (unsigned int)i,
                 (unsigned long)snapshot[i].id,
                 i == 0 ? 1 : 0);
        write_message_json_chunk(request, &snapshot[i], i == 0);
    }
    ESP_LOGI(TAG, "messages_handler: sending closing ']'");
    chunk_result = httpd_resp_send_chunk(request, "]", 1);
    ESP_LOGI(TAG, "messages_handler: closing ']' result=%s", esp_err_to_name(chunk_result));
    if (chunk_result != ESP_OK) {
        return chunk_result;
    }
    httpd_resp_set_type(request, "application/json");
    ESP_LOGI(TAG, "messages_handler: sending final NULL chunk");
    chunk_result = httpd_resp_send_chunk(request, NULL, 0);
    ESP_LOGI(TAG, "messages_handler: final NULL chunk result=%s", esp_err_to_name(chunk_result));
    ESP_LOGI(TAG, "/api/messages served: %u messages", (unsigned int)snapshot_count);
    return chunk_result;
}
