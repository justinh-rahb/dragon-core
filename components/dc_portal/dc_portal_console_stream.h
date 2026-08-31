#pragma once

#include "esp_err.h"

#include <stddef.h>

typedef esp_err_t (*dc_portal_console_send_fn)(void *ctx, const char *data,
                                                size_t len);

esp_err_t dc_portal_console_stream(void *ctx, dc_portal_console_send_fn send_chunk);
