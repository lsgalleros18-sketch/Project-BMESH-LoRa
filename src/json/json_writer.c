#include "json/json_writer.h"

#include <stdio.h>
#include <string.h>

#include "esp_log.h"

#include "utils/json_utils.h"

static const char *TAG = "json_writer";

void write_message_json_chunk(httpd_req_t *request, const emergency_message_t *message, bool first)
{
    char escaped_direction[FIELD_LEN * 2];
    char escaped_source[FIELD_LEN * 2];
    char escaped_destination[FIELD_LEN * 2];
    char escaped_type[FIELD_LEN * 2];
    char escaped_priority[FIELD_LEN * 2];
    char escaped_payload[PAYLOAD_LEN * 2];
    char escaped_packet[PACKET_LEN * 2];
    char escaped_thread_key[FIELD_LEN * 2];
    char escaped_status[FIELD_LEN * 2];
    char escaped_sitio[SITIO_LEN * 2];
    char escaped_barangay[BARANGAY_LEN * 2];
    char escaped_municipality[MUNICIPALITY_LEN * 2];

    json_escape_string(escaped_direction, sizeof(escaped_direction), message->direction);
    json_escape_string(escaped_source, sizeof(escaped_source), message->source);
    json_escape_string(escaped_destination, sizeof(escaped_destination), message->destination);
    json_escape_string(escaped_type, sizeof(escaped_type), message->type);
    json_escape_string(escaped_priority, sizeof(escaped_priority), message->priority);
    json_escape_string(escaped_payload, sizeof(escaped_payload), message->payload);
    json_escape_string(escaped_packet, sizeof(escaped_packet), message->packet);
    json_escape_string(escaped_thread_key, sizeof(escaped_thread_key), message->thread_key);
    json_escape_string(escaped_status, sizeof(escaped_status), message->status);
    json_escape_string(escaped_sitio, sizeof(escaped_sitio), message->origin_location.sitio);
    json_escape_string(escaped_barangay, sizeof(escaped_barangay), message->origin_location.barangay);
    json_escape_string(escaped_municipality, sizeof(escaped_municipality), message->origin_location.municipality);

    {
        size_t priority_len = strnlen(message->priority, sizeof(message->priority));
        size_t escaped_priority_len = strnlen(escaped_priority, sizeof(escaped_priority));
        char priority_bytes[FIELD_LEN * 3 + 1];
        size_t dump_len = 0;
        for (size_t i = 0; i < sizeof(message->priority) && dump_len + 4 < sizeof(priority_bytes); i++) {
            unsigned char c = (unsigned char)message->priority[i];
            if (c == '\0') {
                dump_len += snprintf(priority_bytes + dump_len, sizeof(priority_bytes) - dump_len, "\\0");
                break;
            }
            if (c == '\r') {
                dump_len += snprintf(priority_bytes + dump_len, sizeof(priority_bytes) - dump_len, "\\r");
            } else if (c == '\n') {
                dump_len += snprintf(priority_bytes + dump_len, sizeof(priority_bytes) - dump_len, "\\n");
            } else if (c >= 32 && c <= 126) {
                priority_bytes[dump_len++] = (char)c;
                priority_bytes[dump_len] = '\0';
            } else {
                dump_len += snprintf(priority_bytes + dump_len, sizeof(priority_bytes) - dump_len, "\\x%02X", c);
            }
        }
        priority_bytes[sizeof(priority_bytes) - 1] = '\0';
        ESP_LOGI(TAG, "priority field: raw_len=%u escaped_len=%u raw='%s' escaped='%s'",
                 (unsigned int)priority_len,
                 (unsigned int)escaped_priority_len,
                 priority_bytes,
                 escaped_priority);
        ESP_LOGI(TAG, "priority field: raw buffer terminator=%s", memchr(message->priority, '\0', sizeof(message->priority)) != NULL ? "yes" : "no");
    }

    ESP_LOGI(TAG, "write_message_json_chunk: begin id=%lu first=%d direction='%s' source='%s' destination='%s' type='%s' priority='%s' status='%s' thread_key='%s' payload_len=%u packet_len=%u",
             (unsigned long)message->id,
             first ? 1 : 0,
             message->direction,
             message->source,
             message->destination,
             message->type,
             message->priority,
             message->status,
             message->thread_key,
             (unsigned int)strlen(message->payload),
             (unsigned int)strlen(message->packet));
    esp_err_t chunk_result = httpd_resp_send_chunk(request, first ? "" : ",", HTTPD_RESP_USE_STRLEN);
    ESP_LOGI(TAG, "write_message_json_chunk: prefix result=%s", esp_err_to_name(chunk_result));
    if (chunk_result != ESP_OK) return;
    chunk_result = httpd_resp_send_chunk(request, "{\"id\":", HTTPD_RESP_USE_STRLEN);
    ESP_LOGI(TAG, "write_message_json_chunk: object start result=%s", esp_err_to_name(chunk_result));
    if (chunk_result != ESP_OK) return;
    char id_chunk[24];
    snprintf(id_chunk, sizeof(id_chunk), "%lu", (unsigned long)message->id);
    chunk_result = httpd_resp_send_chunk(request, id_chunk, HTTPD_RESP_USE_STRLEN);
    ESP_LOGI(TAG, "write_message_json_chunk: id value result=%s", esp_err_to_name(chunk_result));
    if (chunk_result != ESP_OK) return;
    chunk_result = httpd_resp_send_chunk(request, ",\"direction\":\"", HTTPD_RESP_USE_STRLEN);
    ESP_LOGI(TAG, "write_message_json_chunk: direction prefix result=%s", esp_err_to_name(chunk_result));
    if (chunk_result != ESP_OK) return;
    chunk_result = httpd_resp_send_chunk(request, escaped_direction, HTTPD_RESP_USE_STRLEN);
    ESP_LOGI(TAG, "write_message_json_chunk: direction value result=%s", esp_err_to_name(chunk_result));
    if (chunk_result != ESP_OK) return;
    chunk_result = httpd_resp_send_chunk(request, "\",\"source\":\"", HTTPD_RESP_USE_STRLEN);
    ESP_LOGI(TAG, "write_message_json_chunk: source prefix result=%s", esp_err_to_name(chunk_result));
    if (chunk_result != ESP_OK) return;
    chunk_result = httpd_resp_send_chunk(request, escaped_source, HTTPD_RESP_USE_STRLEN);
    ESP_LOGI(TAG, "write_message_json_chunk: source value result=%s", esp_err_to_name(chunk_result));
    if (chunk_result != ESP_OK) return;
    chunk_result = httpd_resp_send_chunk(request, "\",\"destination\":\"", HTTPD_RESP_USE_STRLEN);
    ESP_LOGI(TAG, "write_message_json_chunk: destination prefix result=%s", esp_err_to_name(chunk_result));
    if (chunk_result != ESP_OK) return;
    chunk_result = httpd_resp_send_chunk(request, escaped_destination, HTTPD_RESP_USE_STRLEN);
    ESP_LOGI(TAG, "write_message_json_chunk: destination value result=%s", esp_err_to_name(chunk_result));
    if (chunk_result != ESP_OK) return;
    chunk_result = httpd_resp_send_chunk(request, "\",\"type\":\"", HTTPD_RESP_USE_STRLEN);
    ESP_LOGI(TAG, "write_message_json_chunk: type prefix result=%s", esp_err_to_name(chunk_result));
    if (chunk_result != ESP_OK) return;
    chunk_result = httpd_resp_send_chunk(request, escaped_type, HTTPD_RESP_USE_STRLEN);
    ESP_LOGI(TAG, "write_message_json_chunk: type value result=%s", esp_err_to_name(chunk_result));
    if (chunk_result != ESP_OK) return;
    {
        const char *priority_prefix = "\",\"priority\":\"";
        ESP_LOGI(TAG, "write_message_json_chunk: priority prefix len=%u text='%s'",
                 (unsigned int)strlen(priority_prefix),
                 priority_prefix);
        chunk_result = httpd_resp_send_chunk(request, priority_prefix, HTTPD_RESP_USE_STRLEN);
    }
    ESP_LOGI(TAG, "write_message_json_chunk: priority prefix result=%s", esp_err_to_name(chunk_result));
    if (chunk_result != ESP_OK) return;
    ESP_LOGI(TAG, "write_message_json_chunk: priority value len=%u text='%s'",
             (unsigned int)strlen(escaped_priority),
             escaped_priority);
    chunk_result = httpd_resp_send_chunk(request, escaped_priority, HTTPD_RESP_USE_STRLEN);
    ESP_LOGI(TAG, "write_message_json_chunk: priority value result=%s", esp_err_to_name(chunk_result));
    if (chunk_result != ESP_OK) return;
    chunk_result = httpd_resp_send_chunk(request, "\",\"payload\":\"", HTTPD_RESP_USE_STRLEN);
    ESP_LOGI(TAG, "write_message_json_chunk: payload prefix result=%s", esp_err_to_name(chunk_result));
    if (chunk_result != ESP_OK) return;
    chunk_result = httpd_resp_send_chunk(request, escaped_payload, HTTPD_RESP_USE_STRLEN);
    ESP_LOGI(TAG, "write_message_json_chunk: payload value result=%s", esp_err_to_name(chunk_result));
    if (chunk_result != ESP_OK) return;
    chunk_result = httpd_resp_send_chunk(request, "\",\"packet\":\"", HTTPD_RESP_USE_STRLEN);
    ESP_LOGI(TAG, "write_message_json_chunk: packet prefix result=%s", esp_err_to_name(chunk_result));
    if (chunk_result != ESP_OK) return;
    chunk_result = httpd_resp_send_chunk(request, escaped_packet, HTTPD_RESP_USE_STRLEN);
    ESP_LOGI(TAG, "write_message_json_chunk: packet value result=%s", esp_err_to_name(chunk_result));
    if (chunk_result != ESP_OK) return;
    chunk_result = httpd_resp_send_chunk(request, "\",\"thread_key\":\"", HTTPD_RESP_USE_STRLEN);
    ESP_LOGI(TAG, "write_message_json_chunk: thread_key prefix result=%s", esp_err_to_name(chunk_result));
    if (chunk_result != ESP_OK) return;
    chunk_result = httpd_resp_send_chunk(request, escaped_thread_key, HTTPD_RESP_USE_STRLEN);
    ESP_LOGI(TAG, "write_message_json_chunk: thread_key value result=%s", esp_err_to_name(chunk_result));
    if (chunk_result != ESP_OK) return;
    chunk_result = httpd_resp_send_chunk(request, "\",\"status\":\"", HTTPD_RESP_USE_STRLEN);
    ESP_LOGI(TAG, "write_message_json_chunk: status prefix result=%s", esp_err_to_name(chunk_result));
    if (chunk_result != ESP_OK) return;
    chunk_result = httpd_resp_send_chunk(request, escaped_status, HTTPD_RESP_USE_STRLEN);
    ESP_LOGI(TAG, "write_message_json_chunk: status value result=%s", esp_err_to_name(chunk_result));
    if (chunk_result != ESP_OK) return;
    chunk_result = httpd_resp_send_chunk(request, "\",\"rssi\":", HTTPD_RESP_USE_STRLEN);
    ESP_LOGI(TAG, "write_message_json_chunk: rssi prefix result=%s", esp_err_to_name(chunk_result));
    if (chunk_result != ESP_OK) return;
    char rssi_chunk[16];
    snprintf(rssi_chunk, sizeof(rssi_chunk), "%d", message->rssi);
    chunk_result = httpd_resp_send_chunk(request, rssi_chunk, HTTPD_RESP_USE_STRLEN);
    ESP_LOGI(TAG, "write_message_json_chunk: rssi value result=%s", esp_err_to_name(chunk_result));
    if (chunk_result != ESP_OK) return;
    chunk_result = httpd_resp_send_chunk(request, ",\"snr\":", HTTPD_RESP_USE_STRLEN);
    ESP_LOGI(TAG, "write_message_json_chunk: snr prefix result=%s", esp_err_to_name(chunk_result));
    if (chunk_result != ESP_OK) return;
    char snr_chunk[16];
    snprintf(snr_chunk, sizeof(snr_chunk), "%d", message->snr);
    chunk_result = httpd_resp_send_chunk(request, snr_chunk, HTTPD_RESP_USE_STRLEN);
    ESP_LOGI(TAG, "write_message_json_chunk: snr value result=%s", esp_err_to_name(chunk_result));
    if (chunk_result != ESP_OK) return;
    chunk_result = httpd_resp_send_chunk(request, ",\"hops\":", HTTPD_RESP_USE_STRLEN);
    ESP_LOGI(TAG, "write_message_json_chunk: hops prefix result=%s", esp_err_to_name(chunk_result));
    if (chunk_result != ESP_OK) return;
    char hops_chunk[16];
    snprintf(hops_chunk, sizeof(hops_chunk), "%d", message->hops);
    chunk_result = httpd_resp_send_chunk(request, hops_chunk, HTTPD_RESP_USE_STRLEN);
    ESP_LOGI(TAG, "write_message_json_chunk: hops value result=%s", esp_err_to_name(chunk_result));
    if (chunk_result != ESP_OK) return;
    chunk_result = httpd_resp_send_chunk(request, ",\"stored_epoch\":", HTTPD_RESP_USE_STRLEN);
    ESP_LOGI(TAG, "write_message_json_chunk: stored_epoch prefix result=%s", esp_err_to_name(chunk_result));
    if (chunk_result != ESP_OK) return;
    char epoch_chunk[24];
    snprintf(epoch_chunk, sizeof(epoch_chunk), "%lu", (unsigned long)message->stored_epoch);
    chunk_result = httpd_resp_send_chunk(request, epoch_chunk, HTTPD_RESP_USE_STRLEN);
    ESP_LOGI(TAG, "write_message_json_chunk: stored_epoch value result=%s", esp_err_to_name(chunk_result));
    if (chunk_result != ESP_OK) return;
    chunk_result = httpd_resp_send_chunk(request, ",\"location\":{\"sitio\":\"", HTTPD_RESP_USE_STRLEN);
    ESP_LOGI(TAG, "write_message_json_chunk: location prefix result=%s", esp_err_to_name(chunk_result));
    if (chunk_result != ESP_OK) return;
    chunk_result = httpd_resp_send_chunk(request, escaped_sitio, HTTPD_RESP_USE_STRLEN);
    ESP_LOGI(TAG, "write_message_json_chunk: sitio value result=%s", esp_err_to_name(chunk_result));
    if (chunk_result != ESP_OK) return;
    chunk_result = httpd_resp_send_chunk(request, "\",\"barangay\":\"", HTTPD_RESP_USE_STRLEN);
    ESP_LOGI(TAG, "write_message_json_chunk: barangay prefix result=%s", esp_err_to_name(chunk_result));
    if (chunk_result != ESP_OK) return;
    chunk_result = httpd_resp_send_chunk(request, escaped_barangay, HTTPD_RESP_USE_STRLEN);
    ESP_LOGI(TAG, "write_message_json_chunk: barangay value result=%s", esp_err_to_name(chunk_result));
    if (chunk_result != ESP_OK) return;
    chunk_result = httpd_resp_send_chunk(request, "\",\"municipality\":\"", HTTPD_RESP_USE_STRLEN);
    ESP_LOGI(TAG, "write_message_json_chunk: municipality prefix result=%s", esp_err_to_name(chunk_result));
    if (chunk_result != ESP_OK) return;
    chunk_result = httpd_resp_send_chunk(request, escaped_municipality, HTTPD_RESP_USE_STRLEN);
    ESP_LOGI(TAG, "write_message_json_chunk: municipality value result=%s", esp_err_to_name(chunk_result));
    if (chunk_result != ESP_OK) return;
    chunk_result = httpd_resp_send_chunk(request, "\"}}", HTTPD_RESP_USE_STRLEN);
    ESP_LOGI(TAG, "write_message_json_chunk: closing object result=%s", esp_err_to_name(chunk_result));
}
