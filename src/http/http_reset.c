#include "http/http_reset.h"

#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "http/http_auth.h"
#include "http/http_portal.h"

esp_err_t http_reset_handler(httpd_req_t *request)
{
    const http_reset_context_t *context = request->user_ctx;
    esp_err_t session_result = http_auth_require_session(request);

    if (session_result != ESP_OK) {
        return session_result;
    }

    context->erase_node_config();
    http_portal_send_file(request, "reboot.html");
    vTaskDelay(pdMS_TO_TICKS(500));
    esp_restart();
    return ESP_OK;
}
