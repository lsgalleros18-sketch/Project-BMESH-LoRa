#include "http/http_portal.h"

#include <stdio.h>

#include "esp_log.h"

#include "http/http_auth.h"

#define PORTAL_FILE_BUFFER_SIZE 1536

static const char *TAG = "mesh_portal";
static http_portal_context_t portal_context;

void http_portal_init(const http_portal_context_t *context)
{
    portal_context = *context;
}

esp_err_t http_portal_send_file(httpd_req_t *request, const char *filename)
{
    static const char unavailable_html[] =
        "<!doctype html><html><body><h2>Portal unavailable</h2><p>The portal files could not be mounted. Restart the node or reflash its filesystem image.</p></body></html>";
    char path[64];
    char buffer[PORTAL_FILE_BUFFER_SIZE];
    FILE *file;
    esp_err_t result = ESP_OK;
    size_t bytes_read;

    httpd_resp_set_type(request, "text/html");
    if (!*portal_context.littlefs_mounted) {
        httpd_resp_set_status(request, "503 Service Unavailable");
        return httpd_resp_send(request, unavailable_html, HTTPD_RESP_USE_STRLEN);
    }
    if (snprintf(path, sizeof(path), "%s/%s", portal_context.littlefs_base_path, filename) >= (int)sizeof(path)) {
        return ESP_FAIL;
    }
    file = fopen(path, "r");
    if (file == NULL) {
        ESP_LOGE(TAG, "Portal file not found: %s", path);
        httpd_resp_set_status(request, "500 Internal Server Error");
        return httpd_resp_send(request, unavailable_html, HTTPD_RESP_USE_STRLEN);
    }
    while ((bytes_read = fread(buffer, 1, sizeof(buffer), file)) > 0) {
        result = httpd_resp_send_chunk(request, buffer, bytes_read);
        if (result != ESP_OK) {
            break;
        }
    }
    if (ferror(file)) {
        result = ESP_FAIL;
    }
    fclose(file);
    return result == ESP_OK ? httpd_resp_send_chunk(request, NULL, 0) : result;
}

esp_err_t http_portal_index_handler(httpd_req_t *request)
{
    if (!*portal_context.configured) {
        return http_portal_send_file(request, "setup.html");
    }
    if (!http_auth_request_has_session(request)) {
        return http_portal_send_file(request, "login.html");
    }

    return http_portal_send_file(request, "index.html");
}

esp_err_t http_portal_captive_handler(httpd_req_t *request)
{
    return http_auth_send_redirect(request, "/");
}
