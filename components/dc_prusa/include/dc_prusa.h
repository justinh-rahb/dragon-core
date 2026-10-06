#pragma once

// PrusaLink HTTP client (Prusa Core One / Buddy firmware). Polls
// `GET http://<host>/api/v1/status` over plain HTTP with an `X-Api-Key` header,
// caches the bed temperature/target + printer state, and — while a print is
// active — additionally polls for the printing filament type so AUTO can follow
// the filament like the Bambu and Moonraker sources. When no material is reported,
// `material` is left empty and the product's AUTO stays idle (filament-follow only;
// there is no bed-follow fallback). Idle (no-op) if no host is configured.
// READ-ONLY — never commands the printer. The first `esp_http_client` control
// source in the codebase.
//
// Contract verified against the Prusa-Link-Web OpenAPI specs and
// Prusa-Firmware-Buddy source (lib/WUI/nhttp/status_renderer.cpp; auth in
// tests/integration/test_prusa_link.py):
//   /api/v1/status -> printer.temp_bed / printer.target_bed (floats),
//                     printer.state (enum string)    [modern v1 API]
//   /api/v1/job    -> file.meta."filament_type" (string) and, for multi-toolhead
//                     printers (e.g. XL), "filament_type per tool" +
//                     "filament used [mm] per tool" arrays (the printed slot is
//                     the one with non-zero usage, mirroring dc_moonraker #65).
//                     [modern v1 API — but current Buddy firmware does not emit
//                      file.meta, so this usually yields nothing; see the fallback]
//   /api/printer   -> telemetry.material (string)    [LEGACY OctoPrint-compatible
//                     API — the material fallback, and the only endpoint that
//                     reliably reports the loaded filament on Buddy firmware today]
//   X-Api-Key = the PrusaLink password, 401 on mismatch.

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
    char  material[24];         // printing filament type from /api/v1/job
                                // (file.meta.filament_type); empty when not
                                // printing or the job has no filament metadata
    uint32_t status_age_ms;      // age of last complete status; UINT32_MAX if unavailable
} dc_prusa_status_t;

esp_err_t dc_prusa_start(void);

// Overwrite the saved config. Safe before dc_prusa_start(); if the poller is
// running it picks up the new settings on the next cycle.
esp_err_t dc_prusa_set_config(const dc_prusa_config_t *cfg);

// Persisted config, readable even before dc_prusa_start().
esp_err_t dc_prusa_get_config(dc_prusa_config_t *out);
// Returns a freshness-filtered snapshot. If no complete status has arrived within
// 15 seconds, online is false, an ONLINE/CONNECTING state is reported as OFFLINE,
// bed_temp is NaN, bed_target is 0, and printer_state and material are empty.
esp_err_t dc_prusa_get_status(dc_prusa_status_t *out);

// Wipe saved PrusaLink config (factory reset).
esp_err_t dc_prusa_clear_config(void);

// AUTO policy lives in the product (app_main): when `material` is reported it
// follows the filament's chamber zone (like Bambu/Moonraker); when it is absent or
// has no configured zone, AUTO stays idle (filament-follow only — no bed-follow).
// This component still reports bed_temp / bed_target / state for display and for any
// future bed-based rule, plus `material`.

#define DC_PRUSA_DEFAULT_PORT 80
