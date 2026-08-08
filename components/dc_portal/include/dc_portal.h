#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "cJSON.h"
#include "esp_err.h"
#include "esp_http_server.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef cJSON *(*dc_portal_describe_fn)(void *ctx);
typedef esp_err_t (*dc_portal_apply_fn)(const cJSON *values, void *ctx,
                                        char *message, size_t message_size);
typedef bool (*dc_portal_authorize_fn)(httpd_req_t *req, void *ctx);
typedef esp_err_t (*dc_portal_reset_fn)(void *ctx);

typedef struct {
    const char *product;
    const char *display_name;
    const httpd_uri_t *product_routes;
    size_t product_route_count;
    dc_portal_describe_fn describe_product;
    dc_portal_apply_fn apply_product;
    dc_portal_authorize_fn authorize;
    dc_portal_reset_fn factory_reset;
    void *ctx;
} dc_portal_config_t;

// Starts the family HTTP/provisioning service. The service owns the HTTP server,
// the shared SPA and captive DNS. Product routes are registered before the
// catch-all and remain responsible for device-specific API behavior.
esp_err_t dc_portal_start(const dc_portal_config_t *config);
esp_err_t dc_portal_stop(void);
httpd_handle_t dc_portal_httpd(void);

#ifdef __cplusplus
}
#endif
