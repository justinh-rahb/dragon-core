#pragma once

// Read-only HTTP export of the dc_evlog raw console byte ring, addressed by an
// absolute byte cursor. Board- and portal-neutral: the product owns the
// esp_http_server instance, the boot identity and the authorization policy.
//
// Wire contract (see README.md for the full description):
//
//   GET /api/v1/system/log?cursor=N
//
//   200  body == raw ring bytes in [start_seq, end_seq), byte-exact,
//        Content-Type application/octet-stream; next cursor = end_seq
//   400  missing / malformed cursor or unexpected query parameter
//   403  product authorization policy refused the request
//   409  cursor > write_seq: not a valid position in this boot
//
// The exporter keeps no per-client state, never queues, never logs from its
// request path, and never holds the console lock across socket I/O.

#include <stdbool.h>
#include <stddef.h>

#include "esp_err.h"
#include "esp_http_server.h"

#include "dc_logtail_limits.h"

#ifdef __cplusplus
extern "C" {
#endif

// Same shape as dc_portal_authorize_fn, so a product can pass its existing
// policy function, but dc_logtail does not depend on dc_portal.
typedef bool (*dc_logtail_authorize_fn)(httpd_req_t *req, void *ctx);

typedef struct {
    // Required. Product-owned identity of the current boot/session: the same
    // value the product exposes on its other evidence/state surfaces. 1 to
    // DC_LOGTAIL_BOOT_ID_MAX characters from [A-Za-z0-9._:-]. Copied at
    // registration, so the caller's storage need not outlive the call.
    const char *boot_id;

    // Exactly one of the following must be set. A zero-initialised config is
    // rejected: a NULL callback never implies public access.
    dc_logtail_authorize_fn authorize;  // product policy; true = allow
    bool allow_unauthenticated;         // deliberate opt-in to public export
    void *auth_ctx;                     // passed to authorize
} dc_logtail_config_t;

// Registers GET DC_LOGTAIL_URI on `server`. Register before any wildcard
// catch-all GET route (with dc_portal: from register_product_routes).
//
// The first successful call latches the configuration for the rest of the
// boot. Later calls, e.g. after the product restarts its HTTP server, must
// pass an identical configuration; anything else returns
// ESP_ERR_INVALID_STATE so the boot identity can never change mid-boot.
//
// Returns ESP_ERR_INVALID_ARG for a NULL server or an invalid config, or the
// esp_http_server registration error.
esp_err_t dc_logtail_register(httpd_handle_t server,
                              const dc_logtail_config_t *config);

#ifdef __cplusplus
}
#endif
