#include "network/http_request.h"

#include <string.h>

esp_err_t http_read_body_checked(httpd_req_t *req, char *buffer, size_t buffer_size)
{
    int received = 0;

    if (req == NULL || buffer == NULL || buffer_size == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    if (req->content_len >= buffer_size) {
        httpd_resp_set_status(req, "413 Payload Too Large");
        httpd_resp_send(req, "Request body too large", HTTPD_RESP_USE_STRLEN);
        return ESP_ERR_INVALID_SIZE;
    }

    memset(buffer, 0, buffer_size);
    while (received < req->content_len) {
        int ret = httpd_req_recv(req, buffer + received, req->content_len - received);
        if (ret <= 0) {
            return ESP_FAIL;
        }
        received += ret;
    }

    buffer[received] = '\0';
    return ESP_OK;
}
