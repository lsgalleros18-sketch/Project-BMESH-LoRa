#pragma once

#include "http/http_messages.h"
#include "http/http_reset.h"
#include "http/http_send.h"
#include "http/http_setup.h"
#include "http/http_status.h"
#include "http/http_sync.h"
#include "http/http_time.h"

void http_server_start(const http_messages_context_t *messages_context,
                      const http_status_context_t *status_context,
                      const http_time_context_t *time_context,
                      const http_sync_context_t *sync_context,
                      const http_reset_context_t *reset_context,
                      const http_send_context_t *send_context,
                      const http_setup_context_t *setup_context);
