// SPDX-License-Identifier: MIT
#pragma once
//
// dc_breath_link — light-touch client that polls a DragonBreath's read-only
// HTTP state (`GET /api/v2/state`, ~60 s) and exposes a mutex-guarded snapshot of
// its heater state for consumers (e.g. DragonVent AUTO) to use as an ADVISORY
// information source.
//
// It is never a control source: opt-in per device, advisory only, and fail-safe —
// a consumer that finds the snapshot stale or absent must fall back to its own
// primary logic. All network work runs on one dedicated low-priority task on a
// gentle cadence, so it never starves the single-core WiFi/httpd. See
// docs/rfc-vent-breath-link.md.

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include "esp_err.h"

#define DC_BREATH_ADDR_MAX  64   // host or IP, e.g. "dragonbreath.local"
#define DC_BREATH_MODE_MAX  12   // "off" / "power_on" / "auto" / "drying"

// Poll cadence and the fresh-window past which a snapshot counts as "no-signal".
#define DC_BREATH_POLL_MS   60000
#define DC_BREATH_FRESH_US  (150LL * 1000000LL)   // ~2-3 missed polls

// Persisted configuration (NVS namespace "app_nvs").
typedef struct {
    bool enabled;                       // info source on/off (default false)
    char address[DC_BREATH_ADDR_MAX];   // Breath host/IP; empty => not configured
} dc_breath_link_config_t;

// Last-known Breath heater state. `valid` stays false until the first good poll.
typedef struct {
    bool     valid;
    int64_t  updated_us;                // esp_timer_get_time() of the last good poll
    bool     connected;                 // reachable on the last attempt
    char     mode[DC_BREATH_MODE_MAX];  // off / power_on / auto / drying
    float    target_c;                  // target.effective_c (fallback requested_c)
    float    chamber_c;                 // sensors.chamber.temperature_c (display)
    bool     demand;                    // heater.demand
    bool     fault;                     // safety.fault_latched
    bool     inhibited;                 // safety.inhibited
    uint32_t state_revision;
} dc_breath_snapshot_t;

// Load config from NVS and start the (idle-until-needed) poll task. Idempotent.
esp_err_t dc_breath_link_start(void);

// Persist config to NVS and apply live (begins/ends polling as appropriate).
esp_err_t dc_breath_link_set_config(const dc_breath_link_config_t *cfg);
esp_err_t dc_breath_link_get_config(dc_breath_link_config_t *out);

// True when enabled AND a non-empty address is set.
bool dc_breath_link_configured(void);

// Consumers gate polling to when they actually need Breath data (e.g. AUTO mode).
// Polling only runs while active && configured; otherwise the task sleeps.
void dc_breath_link_set_active(bool active);

// Copy the current snapshot. Returns true if a snapshot has ever been taken
// (does not imply freshness — check `updated_us` against DC_BREATH_FRESH_US).
bool dc_breath_link_get(dc_breath_snapshot_t *out);

// Convenience for the primary consumer rule: is the Breath actively running a
// heating job, per a FRESH snapshot? Encapsulates:
//   configured && fresh && connected && !fault && !inhibited && target_c > 0
//   && mode in { power_on, auto, drying }
// Returns false whenever the Breath is unconfigured, stale, or not heating —
// so a consumer can treat "false" as "no reason to seal from the Breath".
bool dc_breath_link_heater_running(void);

// Erase persisted config (for a factory reset) and drop any cached snapshot.
esp_err_t dc_breath_link_clear_config(void);
