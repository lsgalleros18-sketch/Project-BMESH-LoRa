#include "http/http_send.h"

#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "bems_common.h"
#include "http/http_auth.h"
#include "messages/message_store.h"
#include "utils/string_utils.h"

static TickType_t last_send_tick;

esp_err_t http_send_handler(httpd_req_t *request)
{
    const http_send_context_t *context = request->user_ctx;
    char body[384] = {0};
    char destination[FIELD_LEN];
    char sender_name[FIELD_LEN] = {0};
    char type[FIELD_LEN] = "TEST";
    char priority[FIELD_LEN] = "NORMAL";
    char payload[PAYLOAD_LEN] = "No message";
    int received = 0;
    esp_err_t session_result = http_auth_require_session(request);

    if (session_result != ESP_OK) {
        return session_result;
    }

    while (received < request->content_len && received < (int)sizeof(body) - 1) {
        int ret = httpd_req_recv(request, body + received, MIN(request->content_len - received, (int)sizeof(body) - 1 - received));
        if (ret <= 0) {
            return ESP_FAIL;
        }
        received += ret;
    }

    copy_field(destination, sizeof(destination), context->default_destination);
    form_value(body, "sender_name", sender_name, sizeof(sender_name));
    form_value(body, "destination", destination, sizeof(destination));
    copy_field_no_delims(destination, sizeof(destination), destination);
    form_value(body, "type", type, sizeof(type));
    copy_field_no_delims(type, sizeof(type), type);
    form_value(body, "priority", priority, sizeof(priority));
    copy_field_no_delims(priority, sizeof(priority), priority);
    form_value(body, "payload", payload, sizeof(payload));

    if ((xTaskGetTickCount() - last_send_tick) < pdMS_TO_TICKS(10000)) {
        httpd_resp_set_status(request, "429 Too Many Requests");
        httpd_resp_set_type(request, "text/html");
        return httpd_resp_send(request, "<!doctype html><html><body><h2>Slow down.</h2><p>Please wait before sending again.</p></body></html>", HTTPD_RESP_USE_STRLEN);
    }

    last_send_tick = xTaskGetTickCount();
    if (sender_name[0] != '\0') {
        char named_payload[PAYLOAD_LEN];
        size_t prefix_len;

        named_payload[0] = '\0';
        copy_field(named_payload, sizeof(named_payload), sender_name);
        prefix_len = strlen(named_payload);
        if (prefix_len < sizeof(named_payload) - 3) {
            named_payload[prefix_len++] = ':';
            named_payload[prefix_len++] = ' ';
            named_payload[prefix_len] = '\0';
            copy_field(named_payload + prefix_len, sizeof(named_payload) - prefix_len, payload);
        }
        copy_field(payload, sizeof(payload), named_payload);
    }

    context->queue_message(destination, type, priority, payload);
    return http_auth_send_redirect(request, "/");
}
