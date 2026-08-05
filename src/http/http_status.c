#include "http/http_status.h"

#include <stddef.h>
#include <stdio.h>

#include "esp_log.h"
#include "esp_wifi.h"

#include "bems_common.h"
#include "messages/message_store.h"
#include "roster.h"
#include "route_table.h"
#include "utils/json_utils.h"

static const char *TAG = "mesh_portal";
static roster_entry_t roster_snapshot_buffer[MAX_ROSTER_ENTRIES];
static route_entry_t route_snapshot_buffer[MAX_ROUTE_ENTRIES];

static void send_roster_json_chunk(httpd_req_t *request, const roster_entry_t *entry, bool first)
{
    char escaped_id[FIELD_LEN * 2];
    char escaped_sitio[SITIO_LEN * 2];
    char escaped_barangay[BARANGAY_LEN * 2];
    char escaped_municipality[MUNICIPALITY_LEN * 2];
    char chunk[512];

    json_escape_string(escaped_id, sizeof(escaped_id), entry->node_id);
    json_escape_string(escaped_sitio, sizeof(escaped_sitio), entry->location.sitio);
    json_escape_string(escaped_barangay, sizeof(escaped_barangay), entry->location.barangay);
    json_escape_string(escaped_municipality, sizeof(escaped_municipality), entry->location.municipality);

    snprintf(chunk, sizeof(chunk),
             "%s{\"node_id\":\"%s\",\"location\":{\"sitio\":\"%s\",\"barangay\":\"%s\",\"municipality\":\"%s\"},\"last_seen_epoch\":%lu,\"last_seen_tick_ms\":%lu,\"last_rssi\":%d,\"last_snr\":%d,\"online\":%s,\"learned_passively\":%s}",
             first ? "" : ",",
             escaped_id,
             escaped_sitio,
             escaped_barangay,
             escaped_municipality,
             (unsigned long)entry->last_seen_epoch,
             (unsigned long)entry->last_seen_tick_ms,
             entry->last_rssi,
             entry->last_snr,
             entry->online ? "true" : "false",
             entry->learned_passively ? "true" : "false");
    httpd_resp_send_chunk(request, chunk, HTTPD_RESP_USE_STRLEN);
}

static void send_route_json_chunk(httpd_req_t *request, const route_entry_t *entry, bool first)
{
    char escaped_id[FIELD_LEN * 2];
    char chunk[256];

    json_escape_string(escaped_id, sizeof(escaped_id), entry->node_id);
    snprintf(chunk, sizeof(chunk),
             "%s{\"node_id\":\"%s\",\"best_hop_distance\":%d,\"best_rssi\":%d,\"last_seen_tick_ms\":%lu,\"stale\":%s}",
             first ? "" : ",",
             escaped_id,
             entry->best_hop_distance,
             entry->best_rssi,
             (unsigned long)entry->last_seen_tick_ms,
             entry->stale ? "true" : "false");
    httpd_resp_send_chunk(request, chunk, HTTPD_RESP_USE_STRLEN);
}

esp_err_t http_status_handler(httpd_req_t *request)
{
    const http_status_context_t *context = request->user_ctx;
    wifi_sta_list_t clients = {0};
    char escaped_node[FIELD_LEN * 2];
    char escaped_name[FIELD_LEN * 2];
    char escaped_role[FIELD_LEN * 2];
    char escaped_location[FIELD_LEN * 2];
    char escaped_ssid[FIELD_LEN * 2];
    char escaped_relay[8];
    size_t current_message_count;
    size_t roster_count;
    size_t route_count;
    esp_err_t session_result = context->require_session(request);

    if (session_result != ESP_OK) {
        ESP_LOGW(TAG, "/api/status rejected: %s", esp_err_to_name(session_result));
        return session_result;
    }

    esp_wifi_ap_get_sta_list(&clients);
    current_message_count = message_store_count();
    roster_count = roster_get_snapshot(roster_snapshot_buffer, MAX_ROSTER_ENTRIES);
    route_count = route_table_get_snapshot(route_snapshot_buffer, MAX_ROUTE_ENTRIES);

    json_escape_string(escaped_node, sizeof(escaped_node), context->node_id);
    json_escape_string(escaped_name, sizeof(escaped_name), context->node_name);
    json_escape_string(escaped_role, sizeof(escaped_role), context->node_role);
    json_escape_string(escaped_location, sizeof(escaped_location), context->location);
    json_escape_string(escaped_ssid, sizeof(escaped_ssid), context->ssid);
    json_escape_string(escaped_relay, sizeof(escaped_relay), "true");

    httpd_resp_set_type(request, "application/json");
    httpd_resp_send_chunk(request, "{", 1);
    httpd_resp_send_chunk(request, "\"node\":\"", HTTPD_RESP_USE_STRLEN);
    httpd_resp_send_chunk(request, escaped_node, HTTPD_RESP_USE_STRLEN);
    httpd_resp_send_chunk(request, "\",\"name\":\"", HTTPD_RESP_USE_STRLEN);
    httpd_resp_send_chunk(request, escaped_name, HTTPD_RESP_USE_STRLEN);
    httpd_resp_send_chunk(request, "\",\"node_role\":\"", HTTPD_RESP_USE_STRLEN);
    httpd_resp_send_chunk(request, escaped_role, HTTPD_RESP_USE_STRLEN);
    httpd_resp_send_chunk(request, "\",\"location\":\"", HTTPD_RESP_USE_STRLEN);
    httpd_resp_send_chunk(request, escaped_location, HTTPD_RESP_USE_STRLEN);
    httpd_resp_send_chunk(request, "\",\"ssid\":\"", HTTPD_RESP_USE_STRLEN);
    httpd_resp_send_chunk(request, escaped_ssid, HTTPD_RESP_USE_STRLEN);
    char clients_chunk[24];
    snprintf(clients_chunk, sizeof(clients_chunk), "%u", clients.num);
    httpd_resp_send_chunk(request, "\",\"clients\":", HTTPD_RESP_USE_STRLEN);
    httpd_resp_send_chunk(request, clients_chunk, HTTPD_RESP_USE_STRLEN);
    char messages_chunk[24];
    snprintf(messages_chunk, sizeof(messages_chunk), "%u", (unsigned int)current_message_count);
    httpd_resp_send_chunk(request, ",\"messages\":", HTTPD_RESP_USE_STRLEN);
    httpd_resp_send_chunk(request, messages_chunk, HTTPD_RESP_USE_STRLEN);
    httpd_resp_send_chunk(request, ",\"configured\":", HTTPD_RESP_USE_STRLEN);
    httpd_resp_send_chunk(request, *context->configured ? "true" : "false", HTTPD_RESP_USE_STRLEN);
    httpd_resp_send_chunk(request, ",\"relay\":\"", HTTPD_RESP_USE_STRLEN);
    httpd_resp_send_chunk(request, escaped_relay, HTTPD_RESP_USE_STRLEN);
    httpd_resp_send_chunk(request, "\",\"duplicate_warning\":", HTTPD_RESP_USE_STRLEN);
    httpd_resp_send_chunk(request, *context->duplicate_node_id_warning ? "true" : "false", HTTPD_RESP_USE_STRLEN);
    httpd_resp_send_chunk(request, ",\"time_synced\":", HTTPD_RESP_USE_STRLEN);
    httpd_resp_send_chunk(request, *context->time_synced ? "true" : "false", HTTPD_RESP_USE_STRLEN);
    char epoch_chunk[24];
    snprintf(epoch_chunk, sizeof(epoch_chunk), "%lu", (unsigned long)context->current_epoch_seconds());
    httpd_resp_send_chunk(request, ",\"epoch\":", HTTPD_RESP_USE_STRLEN);
    httpd_resp_send_chunk(request, epoch_chunk, HTTPD_RESP_USE_STRLEN);
    httpd_resp_send_chunk(request, ",\"roster\":[", HTTPD_RESP_USE_STRLEN);
    for (size_t i = 0; i < roster_count; i++) {
        send_roster_json_chunk(request, &roster_snapshot_buffer[i], i == 0);
    }
    httpd_resp_send_chunk(request, "],\"routes\":[", HTTPD_RESP_USE_STRLEN);
    for (size_t i = 0; i < route_count; i++) {
        send_route_json_chunk(request, &route_snapshot_buffer[i], i == 0);
    }
    httpd_resp_send_chunk(request, "]}", 2);
    ESP_LOGI(TAG, "/api/status served: messages=%u roster=%u routes=%u", (unsigned int)current_message_count, (unsigned int)roster_count, (unsigned int)route_count);
    return httpd_resp_send_chunk(request, NULL, 0);
}
