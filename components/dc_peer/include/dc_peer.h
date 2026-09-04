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
#include "esp_err.h"

#define DC_PEER_MAGIC     0xD9
#define DC_PEER_VERSION   1
#define DC_PEER_ID_MAX    20     // stable device id, e.g. "dragonbreath-e51d"
#define DC_PEER_PAYLOAD_MAX  64

// Semantic capabilities (RFC 0004). Grows as products publish more.
typedef enum {
    DC_PEER_CAP_HEATER = 1,      // chamber-heater state (dc_peer_heater_t)
} dc_peer_cap_t;

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
