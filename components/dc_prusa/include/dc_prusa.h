#pragma once

// PrusaLink HTTP client (Prusa Core One / Buddy firmware). Polls
// `GET http://<host>/api/v1/status` over plain HTTP with an `X-Api-Key` header,
// caches the bed temperature/target + printer state, and — while a print is
// active — additionally polls `GET /api/v1/job` for the printing filament type
// (`file.meta.filament_type`). AUTO can then follow the filament like the Bambu
// and Moonraker sources, falling back to the bed-threshold rule when no material
// is reported. Idle (no-op) if no host is configured. READ-ONLY — never commands
// the printer. The first `esp_http_client` control source in the codebase.
//
// Both endpoints are the MODERN PrusaLink v1 API (not the legacy OctoPrint-
// compatible `/api/printer`). Contract verified against the Prusa-Link-Web v1
// OpenAPI spec and Prusa-Firmware-Buddy source (lib/WUI/nhttp/status_renderer.cpp;
// auth in tests/integration/test_prusa_link.py):
//   /api/v1/status -> printer.temp_bed / printer.target_bed (floats),
//                     printer.state (enum string)
//   /api/v1/job    -> file.meta."filament_type" (string) and, for multi-toolhead
//                     printers (e.g. XL), "filament_type per tool" +
//                     "filament used [mm] per tool" arrays (the printed slot is
//                     the one with non-zero usage, mirroring dc_moonraker #65).
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
// follows the filament's chamber zone (like Bambu/Moonraker); otherwise it applies
// the bed-follow rule (chamber engages once bed_target reaches the AUTO card's bed
// threshold). This component reports bed_temp / bed_target / state / material.

#define DC_PRUSA_DEFAULT_PORT 80
