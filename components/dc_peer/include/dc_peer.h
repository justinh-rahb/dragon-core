// SPDX-License-Identifier: MIT
#pragma once
//
// dc_peer — Dragon peer-capability transport over ESP-NOW (RFC 0003/0004).
//
// A connectionless, router-free link between powered Dragon products on the same
// Wi-Fi channel. A PROVIDER broadcasts the current value of a semantic capability
// (temperature, heater state, …); a CONSUMER subscribes to a capability and receives
// each frame with the sender's stable peer_id, deciding freshness by LOCAL receipt
// time and owning its own loss/fallback policy (RFC 0004). It carries meaning, not
// GPIO/MCU commands, and never has safety authority.
//
// This is an alternate transport for the same peer semantics `dc_breath_link` already
// implements over HTTP: push instead of poll, MAC/identity instead of IP.

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include "esp_err.h"

#define DC_PEER_MAGIC     0xD9
#define DC_PEER_VERSION   1
#define DC_PEER_ID_MAX    20     // stable device id, e.g. "dragonbreath-e51d"
#define DC_PEER_PAYLOAD_MAX  64

// Semantic capabilities (RFC 0004). Grows as products publish more. Kept < MAX_CAPS
// (see dc_peer.c). Each device broadcasts ANNOUNCE (its descriptor) plus whatever
// status capability matches its product.
typedef enum {
    DC_PEER_CAP_HEATER   = 1,    // chamber-heater state  (dc_peer_heater_t)  — Breath
    DC_PEER_CAP_ANNOUNCE = 2,    // device descriptor     (dc_peer_announce_t) — all
    DC_PEER_CAP_VENT     = 3,    // enclosure-vent state  (dc_peer_vent_t)    — Vent
    DC_PEER_CAP_DRYER    = 4,    // filament-dryer state  (dc_peer_dryer_t)   — Wheeze
} dc_peer_cap_t;

// A capability id as a bit, for the ANNOUNCE `caps` bitmask.
#define DC_PEER_CAP_BIT(cap)  ((uint16_t)(1u << (cap)))

// Product kinds, carried in the ANNOUNCE descriptor so a console can label a device
// without parsing its peer_id. Stable across the family.
typedef enum {
    DC_PEER_KIND_UNKNOWN = 0,
    DC_PEER_KIND_BREATH  = 1,
    DC_PEER_KIND_VENT    = 2,
    DC_PEER_KIND_WHEEZE  = 3,
    DC_PEER_KIND_TOUCH   = 4,
} dc_peer_kind_t;

// Map a peer id to its product kind by the "dragon<kind>-<hex>" id convention every
// provider follows, or DC_PEER_KIND_UNKNOWN if the id is not a known Dragon-family id.
// Discovery uses this to clamp to expected devices and ignore any other ESP-NOW sender
// that happens to speak the dc_peer envelope — a peer_id is self-reported and carries
// no authentication, so this is a name filter, not identity proof.
static inline dc_peer_kind_t dc_peer_kind_from_id(const char *id)
{
    if (!id) return DC_PEER_KIND_UNKNOWN;
    if (strncmp(id, "dragonbreath-", 13) == 0) return DC_PEER_KIND_BREATH;
    if (strncmp(id, "dragonvent-",   11) == 0) return DC_PEER_KIND_VENT;
    if (strncmp(id, "dragonwheeze-", 13) == 0) return DC_PEER_KIND_WHEEZE;
    if (strncmp(id, "dragontouch-",  12) == 0) return DC_PEER_KIND_TOUCH;
    return DC_PEER_KIND_UNKNOWN;
}

// Wire header; the capability payload (payload_len bytes) follows immediately.
// Packed + fixed-endian fields so it is portable across ESP32 variants.
typedef struct __attribute__((packed)) {
    uint8_t  magic;              // DC_PEER_MAGIC
    uint8_t  version;            // DC_PEER_VERSION
    uint8_t  capability;         // dc_peer_cap_t
    uint8_t  payload_len;
    uint32_t seq;                // provider's monotonically increasing counter
    char     peer_id[DC_PEER_ID_MAX];  // NUL-padded
} dc_peer_hdr_t;

// Heater capability payload. Temperatures are °C ×10 (deci-degrees); target 0 means
// "no active target"; chamber DC_PEER_TEMP_UNKNOWN means "not observable right now".
#define DC_PEER_TEMP_UNKNOWN  INT16_MIN
typedef struct __attribute__((packed)) {
    uint8_t  mode;               // 0 off, 1 power_on, 2 auto, 3 drying
    uint8_t  flags;              // bit0 demand, bit1 fault, bit2 inhibited
    int16_t  target_dc;          // target °C ×10 (0 = none)
    int16_t  chamber_dc;         // chamber °C ×10 (DC_PEER_TEMP_UNKNOWN if unknown)
    uint32_t state_revision;
} dc_peer_heater_t;

#define DC_PEER_HEATER_DEMAND     0x01
#define DC_PEER_HEATER_FAULT      0x02
#define DC_PEER_HEATER_INHIBITED  0x04

// ANNOUNCE capability payload — the device's stable descriptor. Broadcast on a slow
// heartbeat (~5 s); a console keys its device table on the header's peer_id and uses
// this for the label, firmware line, and (for later, source-agnostic control) the IP.
#define DC_PEER_NAME_MAX  24         // friendly product name, e.g. "DragonVent"
#define DC_PEER_FW_MAX    16         // firmware version, e.g. a git-describe string
typedef struct __attribute__((packed)) {
    uint8_t  kind;                       // dc_peer_kind_t
    uint8_t  flags;                      // reserved (0); future: accepts-commands
    uint16_t caps;                       // bitmask of DC_PEER_CAP_BIT(...) this device publishes
    uint8_t  ip[4];                      // STA IPv4, big-endian; 0.0.0.0 if none (for later control)
    char     name[DC_PEER_NAME_MAX];     // NUL-padded
    char     fw[DC_PEER_FW_MAX];         // NUL-padded
} dc_peer_announce_t;

// VENT capability payload — enclosure-vent state.
typedef struct __attribute__((packed)) {
    uint8_t  mode;               // 0 auto, 1 manual
    uint8_t  target;             // 0 open, 1 closed
    uint8_t  flags;              // bit0 running (moving), bit1 calibrating
    uint8_t  printer_state;      // dc_peer_printer_t (advisory summary of the vent's source)
    uint32_t state_revision;
} dc_peer_vent_t;

#define DC_PEER_VENT_RUNNING      0x01
#define DC_PEER_VENT_CALIBRATING  0x02

// Compact printer/source summary a Vent republishes for display only.
typedef enum {
    DC_PEER_PRINTER_UNKNOWN  = 0,
    DC_PEER_PRINTER_IDLE     = 1,
    DC_PEER_PRINTER_PRINTING = 2,
    DC_PEER_PRINTER_PAUSED   = 3,
    DC_PEER_PRINTER_ERROR    = 4,
    DC_PEER_PRINTER_OFFLINE  = 5,   // no reliable source (e.g. Standalone / disconnected)
} dc_peer_printer_t;

// DRYER capability payload — filament-dryer state (DragonWheeze). Ambient temp is
// °C ×10 (DC_PEER_TEMP_UNKNOWN if the sniffer has no reading); humidity is whole %RH
// (DC_PEER_RH_UNKNOWN if none).
#define DC_PEER_RH_UNKNOWN  0xFF
typedef struct __attribute__((packed)) {
    uint8_t  mode;               // 0 off, 1 idle, 2 drying
    uint8_t  flags;              // bit0 power on, bit1 active (heating cycle)
    uint8_t  set_temp_c;         // target °C (0 = none)
    uint8_t  set_time_h;         // target hours (0 = none)
    int16_t  ambient_dc;         // ambient °C ×10 (DC_PEER_TEMP_UNKNOWN if unknown)
    uint8_t  humidity_pct;       // %RH (DC_PEER_RH_UNKNOWN if unknown)
    uint8_t  reserved;           // 0
    uint32_t remaining_sec;      // cycle time remaining
    uint32_t elapsed_sec;        // cycle time elapsed
} dc_peer_dryer_t;

#define DC_PEER_DRYER_POWER   0x01
#define DC_PEER_DRYER_ACTIVE  0x02

// Initialise ESP-NOW and the broadcast peer, and adopt `self_id` as this device's
// peer_id on everything it publishes. Call once, AFTER dc_wifi_start(). Idempotent.
// `self_id` is copied; may be NULL for a consumer-only device.
esp_err_t dc_peer_start(const char *self_id);

// PROVIDER: broadcast the current value of a capability. `payload` is the matching
// dc_peer_* struct. Cheap; call on state change and/or a heartbeat.
esp_err_t dc_peer_publish(dc_peer_cap_t cap, const void *payload, size_t len);

// CONSUMER: receive callback, invoked (in the ESP-NOW recv context — keep it fast and
// non-blocking) for each valid frame of `cap`. `peer_id` is the sender's id.
typedef void (*dc_peer_rx_cb_t)(const char *peer_id, dc_peer_cap_t cap,
                                const void *payload, size_t len, void *ctx);

// CONSUMER: subscribe to a capability. One callback per capability (last wins).
esp_err_t dc_peer_subscribe(dc_peer_cap_t cap, dc_peer_rx_cb_t cb, void *ctx);

// Transport diagnostics (for status/telemetry).
typedef struct {
    bool     started;                       // dc_peer_start() succeeded
    uint32_t rx_frames;                     // valid frames received (all capabilities)
    uint32_t tx_frames;                     // frames published
    int64_t  last_rx_us;                    // esp_timer time of last rx (0 = never)
    char     last_peer_id[DC_PEER_ID_MAX];  // sender of the last received frame
} dc_peer_stats_t;

void dc_peer_get_stats(dc_peer_stats_t *out);

// A peer we've recently heard from (for a "pick your device" UI).
typedef struct {
    char    id[DC_PEER_ID_MAX];
    int64_t last_us;   // esp_timer time last heard
} dc_peer_info_t;

// Copy up to `max` recently-heard peers into `out`, most-recent first. Returns count.
int dc_peer_get_peers(dc_peer_info_t *out, int max);
