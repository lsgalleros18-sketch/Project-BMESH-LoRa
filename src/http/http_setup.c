#include "http/http_setup.h"

#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "http/http_portal.h"
#include "utils/string_utils.h"

esp_err_t http_setup_handler(httpd_req_t *request)
{
    const http_setup_context_t *context = request->user_ctx;
    char body[384] = {0};
    char raw_node_id[FIELD_LEN] = {0};
    node_config_t new_config = {0};
    int received = 0;

    while (received < request->content_len && received < (int)sizeof(body) - 1) {
        int ret = httpd_req_recv(request, body + received, MIN(request->content_len - received, (int)sizeof(body) - 1 - received));
        if (ret <= 0) {
            return ESP_FAIL;
        }
        received += ret;
    }

    form_value(body, "node_id", raw_node_id, sizeof(raw_node_id));
    context->copy_node_id(new_config.node_id, sizeof(new_config.node_id), raw_node_id);
    form_value(body, "node_name", new_config.node_name, sizeof(new_config.node_name));
    form_value(body, "node_role", new_config.node_role, sizeof(new_config.node_role));
    form_value(body, "sitio", new_config.location.sitio, sizeof(new_config.location.sitio));
    copy_field_no_delims(new_config.location.sitio, sizeof(new_config.location.sitio), new_config.location.sitio);
    form_value(body, "barangay", new_config.location.barangay, sizeof(new_config.location.barangay));
    copy_field_no_delims(new_config.location.barangay, sizeof(new_config.location.barangay), new_config.location.barangay);
    form_value(body, "municipality", new_config.location.municipality, sizeof(new_config.location.municipality));
    copy_field_no_delims(new_config.location.municipality, sizeof(new_config.location.municipality), new_config.location.municipality);
    form_value(body, "default_destination", new_config.default_destination, sizeof(new_config.default_destination));
    form_value(body, "default_priority", new_config.default_priority, sizeof(new_config.default_priority));
    form_value(body, "ap_password", new_config.ap_password, sizeof(new_config.ap_password));
    form_value(body, "web_pin", new_config.web_pin, sizeof(new_config.web_pin));
    form_value(body, "duress_pin", new_config.duress_pin, sizeof(new_config.duress_pin));
    form_value(body, "network_key", new_config.network_key, sizeof(new_config.network_key));
    new_config.configured = true;
    if (new_config.node_id[0] == '\0') {
        copy_field(new_config.node_id, sizeof(new_config.node_id), context->node_id);
    }
    if (new_config.node_name[0] == '\0') {
        copy_field(new_config.node_name, sizeof(new_config.node_name), "Mesh Node");
    }
    if (new_config.location.barangay[0] == '\0') {
        copy_field(new_config.location.barangay, sizeof(new_config.location.barangay), "Unknown");
    }
    if (new_config.default_destination[0] == '\0') {
        copy_field(new_config.default_destination, sizeof(new_config.default_destination), "BRGY001");
    }
    if (new_config.node_role[0] == '\0') {
        copy_field(new_config.node_role, sizeof(new_config.node_role), "relay-only");
    }
    if (new_config.default_priority[0] == '\0') {
        copy_field(new_config.default_priority, sizeof(new_config.default_priority), "NORMAL");
    }
    if (new_config.ap_password[0] == '\0') {
        copy_field(new_config.ap_password, sizeof(new_config.ap_password), context->ap_password);
    }
    if (new_config.web_pin[0] == '\0') {
        copy_field(new_config.web_pin, sizeof(new_config.web_pin), context->default_web_pin);
    }
    if (new_config.duress_pin[0] == '\0') {
        copy_field(new_config.duress_pin, sizeof(new_config.duress_pin), "");
    }
    if (new_config.network_key[0] == '\0') {
        copy_field(new_config.network_key, sizeof(new_config.network_key), context->default_network_key);
    }

    ESP_ERROR_CHECK(context->save_node_config(&new_config));
    http_portal_send_file(request, "reboot.html");
    vTaskDelay(pdMS_TO_TICKS(500));
    esp_restart();
    return ESP_OK;
}
