// SPDX-License-Identifier: MIT
#pragma once
//
// dc_breath_link — consumes a DragonBreath's heater capability over the ESP-NOW peer
// transport (dc_peer, RFC 0004) and exposes a mutex-guarded snapshot of its heater
// state for consumers (e.g. DragonVent AUTO) to use as an ADVISORY information source.
//
// It is never a control source: opt-in per device, advisory only, and fail-safe — a
// consumer that finds the snapshot stale or absent falls back to its own primary logic.
// The Breath pushes frames (no polling); freshness is decided by LOCAL receipt time.
// A bound peer_id scopes the link to exactly one Breath; empty = unbound (no signal). See
// docs/rfc-vent-breath-link.md.

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include "esp_err.h"
#include "dc_peer.h"   // DC_PEER_ID_MAX

// Which transport last updated the snapshot (only ESP-NOW today; kept for telemetry).
typedef enum {
    DC_BREATH_TX_NONE   = 0,
    DC_BREATH_TX_ESPNOW = 1,
    DC_BREATH_TX_HTTP   = 2,
} dc_breath_transport_t;

#define DC_BREATH_MODE_MAX  12   // "off" / "power_on" / "auto" / "drying"

// Fresh-window past which a snapshot counts as "no-signal".
#define DC_BREATH_FRESH_US  (150LL * 1000000LL)

// Persisted configuration (NVS namespace "app_nvs").
typedef struct {
    bool enabled;                       // info source on/off (default false)
    char peer_id[DC_PEER_ID_MAX];       // bound DragonBreath peer id; empty => unbound (no signal)
} dc_breath_link_config_t;

// Last-known Breath heater state. `valid` stays false until the first frame arrives.
typedef struct {
    bool     valid;
    int64_t  updated_us;                // esp_timer_get_time() of the last good frame
    bool     connected;                 // a frame has been received
    char     mode[DC_BREATH_MODE_MAX];  // off / power_on / auto / drying
    float    target_c;
    float    chamber_c;                 // display; NAN if the Breath can't observe it
    bool     demand;
    bool     fault;
    bool     inhibited;
    uint32_t state_revision;
    uint8_t  transport;                 // dc_breath_transport_t — what last updated this
    char     peer_id[DC_PEER_ID_MAX];   // sender id of the last frame
} dc_breath_snapshot_t;

// Load config from NVS and subscribe to the Breath heater capability. Idempotent.
esp_err_t dc_breath_link_start(void);

// Persist config to NVS and apply live.
esp_err_t dc_breath_link_set_config(const dc_breath_link_config_t *cfg);
esp_err_t dc_breath_link_get_config(dc_breath_link_config_t *out);

// True when the info source is enabled.
bool dc_breath_link_configured(void);

// Copy the current snapshot. Returns true if a snapshot has ever been taken
// (does not imply freshness — check `updated_us` against DC_BREATH_FRESH_US).
bool dc_breath_link_get(dc_breath_snapshot_t *out);

// Convenience for the primary consumer rule: is the Breath actively running a heating
// job, per a FRESH snapshot? Encapsulates:
//   enabled && fresh && !fault && !inhibited && target_c > 0
//   && mode in { power_on, auto, drying }
// False whenever the Breath is disabled, stale, or not heating.
bool dc_breath_link_heater_running(void);

// Erase persisted config (for a factory reset) and drop any cached snapshot.
esp_err_t dc_breath_link_clear_config(void);
