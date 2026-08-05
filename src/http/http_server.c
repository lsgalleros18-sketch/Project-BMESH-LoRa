#include "http/http_server.h"

#include <stddef.h>

#include "esp_http_server.h"

#include "http/http_auth.h"
#include "http/http_portal.h"

#define HTTP_PORT 80

static httpd_handle_t http_server;

void http_server_start(const http_messages_context_t *messages_context,
                       const http_status_context_t *status_context,
                       const http_time_context_t *time_context,
                       const http_sync_context_t *sync_context,
                       const http_reset_context_t *reset_context,
                       const http_send_context_t *send_context,
                       const http_setup_context_t *setup_context)
{
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = HTTP_PORT;
    config.max_uri_handlers = 16;
    config.uri_match_fn = httpd_uri_match_wildcard;
    config.stack_size = 20480;

    const httpd_uri_t routes[] = {
        {.uri = "/", .method = HTTP_GET, .handler = http_portal_index_handler},
        {.uri = "/login", .method = HTTP_POST, .handler = http_auth_login_handler},
        {.uri = "/setup", .method = HTTP_POST, .handler = http_setup_handler, .user_ctx = (void *)setup_context},
        {.uri = "/reset", .method = HTTP_POST, .handler = http_reset_handler, .user_ctx = (void *)reset_context},
        {.uri = "/settime", .method = HTTP_POST, .handler = http_time_handler, .user_ctx = (void *)time_context},
        {.uri = "/send", .method = HTTP_POST, .handler = http_send_handler, .user_ctx = (void *)send_context},
        {.uri = "/sync", .method = HTTP_POST, .handler = http_sync_handler, .user_ctx = (void *)sync_context},
        {.uri = "/api/status", .method = HTTP_GET, .handler = http_status_handler, .user_ctx = (void *)status_context},
        {.uri = "/api/messages", .method = HTTP_GET, .handler = http_messages_handler, .user_ctx = (void *)messages_context},
        {.uri = "/generate_204", .method = HTTP_GET, .handler = http_portal_captive_handler},
        {.uri = "/gen_204", .method = HTTP_GET, .handler = http_portal_captive_handler},
        {.uri = "/hotspot-detect.html", .method = HTTP_GET, .handler = http_portal_captive_handler},
        {.uri = "/connecttest.txt", .method = HTTP_GET, .handler = http_portal_captive_handler},
        {.uri = "/ncsi.txt", .method = HTTP_GET, .handler = http_portal_captive_handler},
        {.uri = "/*", .method = HTTP_GET, .handler = http_portal_captive_handler},
    };

    ESP_ERROR_CHECK(httpd_start(&http_server, &config));
    for (size_t i = 0; i < sizeof(routes) / sizeof(routes[0]); i++) {
        ESP_ERROR_CHECK(httpd_register_uri_handler(http_server, &routes[i]));
    }
}
