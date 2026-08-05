#include "http/http_sync.h"

#include "http/http_auth.h"

esp_err_t http_sync_handler(httpd_req_t *request)
{
    const http_sync_context_t *context = request->user_ctx;
    esp_err_t session_result = http_auth_require_session(request);

    if (session_result != ESP_OK) {
        return session_result;
    }

    context->send_manual_sync_request();
    return http_auth_send_redirect(request, "/");
}
