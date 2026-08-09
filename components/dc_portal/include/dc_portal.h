#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "cJSON.h"
#include "esp_app_desc.h"
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
typedef esp_err_t (*dc_portal_register_fn)(httpd_handle_t server, void *ctx);

typedef enum {
    DC_PORTAL_OPERATION_OTA = 0,
    DC_PORTAL_OPERATION_FACTORY_RESET,
} dc_portal_operation_t;

// Product safety policy runs before core begins a destructive operation. Return
// ESP_ERR_INVALID_STATE for a 409 response, ESP_ERR_INVALID_ARG for 400, or any
// other error for 500. message is returned to the browser when non-empty.
typedef esp_err_t (*dc_portal_guard_fn)(dc_portal_operation_t operation,
                                        void *ctx, char *message,
                                        size_t message_size);

// Called after ESP-IDF has validated and closed an uploaded image but before it
// becomes the boot partition. Products can restrict accepted project identities.
typedef esp_err_t (*dc_portal_validate_image_fn)(const esp_app_desc_t *image,
                                                 void *ctx, char *message,
                                                 size_t message_size);

typedef struct {
    const char *product;
    const char *display_name;
    const httpd_uri_t *product_routes;
    size_t product_route_count;
    dc_portal_register_fn register_product_routes;
    dc_portal_describe_fn describe_product;
    dc_portal_apply_fn apply_product;
    dc_portal_authorize_fn authorize;
    dc_portal_guard_fn guard_operation;
    dc_portal_validate_image_fn validate_image;
    dc_portal_reset_fn factory_reset;
    // Optional product server tuning. Core copies this value, installs wildcard
    // matching, and raises max_uri_handlers when the supplied value is too small.
    const httpd_config_t *httpd_config;
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
