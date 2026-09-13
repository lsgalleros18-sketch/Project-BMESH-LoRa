#include "http/http_time.h"

#include <stdlib.h>

#include "bems_common.h"
#include "http/http_auth.h"
#include "network/http_request.h"
#include "utils/string_utils.h"

esp_err_t http_time_handler(httpd_req_t *request)
{
    const http_time_context_t *context = request->user_ctx;
    char body[128] = {0};
    char epoch_value[FIELD_LEN] = {0};
    uint32_t epoch = 0;
    esp_err_t session_result = http_auth_require_session(request);

    if (session_result != ESP_OK) {
        return session_result;
    }

    if (http_read_body_checked(request, body, sizeof(body)) != ESP_OK) {
        return ESP_FAIL;
    }

    form_value(body, "epoch", epoch_value, sizeof(epoch_value));
    epoch = (uint32_t)strtoul(epoch_value, NULL, 10);
    if (epoch == 0) {
        httpd_resp_set_status(request, "400 Bad Request");
        return httpd_resp_send(request, "Invalid epoch", HTTPD_RESP_USE_STRLEN);
    }

    context->apply_time_sync(epoch, 0);
    context->send_time_sync_packet(epoch, 0, 2);
    return http_auth_send_redirect(request, "/");
}
