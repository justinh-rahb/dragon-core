#pragma once

// PrusaLink HTTP client (Prusa Core One / Buddy firmware). Polls
// `GET http://<host>/api/v1/status` over plain HTTP with an `X-Api-Key` header,
// caches the bed temperature/target + printer state, and derives a chamber
// "follow" target from a bed-threshold rule (PrusaLink exposes no filament type).
// Idle (no-op) if no host is configured. READ-ONLY — never commands the printer.
// The first `esp_http_client` control source in the codebase.
//
// Contract verified against Prusa-Firmware-Buddy source
// (lib/WUI/nhttp/status_renderer.cpp; auth in tests/integration/test_prusa_link.py):
// `printer.temp_bed` / `printer.target_bed` (floats), `printer.state` (enum string),
// X-Api-Key = the PrusaLink password, 401 on mismatch.

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

typedef enum {
    DC_PRUSA_DISABLED = 0,   // no host configured
    DC_PRUSA_CONNECTING,     // configured, no successful poll yet
    DC_PRUSA_ONLINE,         // last poll returned 200 and parsed
    DC_PRUSA_AUTH_FAILED,    // last poll returned 401 (bad/missing X-Api-Key)
    DC_PRUSA_OFFLINE,        // last poll failed (network/timeout/non-200)
} dc_prusa_state_t;

const char *dc_prusa_state_str(dc_prusa_state_t s);

typedef struct {
    char     host[64];    // hostname or IP; empty = unconfigured
    uint16_t port;        // 0 -> default 80
    char     api_key[65]; // PrusaLink password, sent as X-Api-Key
} dc_prusa_config_t;

typedef struct {
    dc_prusa_state_t state;
    bool  online;               // convenience: state == DC_PRUSA_ONLINE
    float bed_temp;             // printer.temp_bed (°C); NaN if never read
    float bed_target;           // printer.target_bed (°C; 0 = bed off)
    char  printer_state[12];    // printer.state ("IDLE"/"PRINTING"/...)
} dc_prusa_status_t;

esp_err_t dc_prusa_start(void);

// Overwrite the saved config. Safe before dc_prusa_start(); if the poller is
// running it picks up the new settings on the next cycle.
esp_err_t dc_prusa_set_config(const dc_prusa_config_t *cfg);

// Persisted config, readable even before dc_prusa_start().
esp_err_t dc_prusa_get_config(dc_prusa_config_t *out);
esp_err_t dc_prusa_get_status(dc_prusa_status_t *out);

// Wipe saved PrusaLink config (factory reset).
esp_err_t dc_prusa_clear_config(void);

// The bed-follow rule (printer bed setpoint -> chamber target) lives in the product
// (app_main), which reads the threshold + target from its AUTO settings: PrusaLink
// reports no filament type, so the AUTO card supplies the bed->chamber rule. This
// component only reports bed_temp / bed_target / state.

#define DC_PRUSA_DEFAULT_PORT 80
