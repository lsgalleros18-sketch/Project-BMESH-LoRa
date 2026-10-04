#include "http/http_time.h"

#include <stdlib.h>
#include <errno.h>

#include "bems_common.h"
#include "http/http_auth.h"
#include "mesh_control.h"
#include "network/http_request.h"
#include "utils/string_utils.h"

#define MIN_PLAUSIBLE_EPOCH 1577836800UL
#define MAX_PLAUSIBLE_EPOCH 4102444800UL
#define MAX_LOCAL_TIME_ADJUSTMENT 86400UL

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
    char *end = NULL;
    errno = 0;
    unsigned long parsed_epoch = strtoul(epoch_value, &end, 10);
    if (end == epoch_value || *end != '\0' || errno == ERANGE || parsed_epoch > UINT32_MAX) parsed_epoch = 0;
    epoch = (uint32_t)parsed_epoch;
    if (epoch < MIN_PLAUSIBLE_EPOCH || epoch > MAX_PLAUSIBLE_EPOCH) {
        httpd_resp_set_status(request, "400 Bad Request");
        return httpd_resp_send(request, "Invalid epoch", HTTPD_RESP_USE_STRLEN);
    }

    if (mesh_control_is_time_synced()) {
        uint32_t current_epoch = mesh_control_current_epoch_seconds();
        uint32_t difference = epoch > current_epoch ? epoch - current_epoch : current_epoch - epoch;
        if (difference > MAX_LOCAL_TIME_ADJUSTMENT) {
            httpd_resp_set_status(request, "400 Bad Request");
            return httpd_resp_send(request, "Time adjustment exceeds allowed bound", HTTPD_RESP_USE_STRLEN);
        }
    }

    context->apply_time_sync(epoch, 0);
    context->send_time_sync_packet(epoch, 0, 2);
    return http_auth_send_redirect(request, "/");
}
