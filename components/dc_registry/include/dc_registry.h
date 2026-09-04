// SPDX-License-Identifier: MIT
#pragma once
//
// dc_registry — a read-only device table for a Dragon console (e.g. DragonTouch).
//
// It subscribes to the dc_peer ANNOUNCE descriptor and the per-product status
// capabilities and keeps one entry per peer_id: the stable descriptor (kind, name,
// firmware, IP) plus the latest status frame for that product. A console renders the
// table; it never sends anything. Freshness is by LOCAL receipt time, per RFC 0004.
//
// Status only: this is discovery + display. Driving a device is a separate, future
// control channel — nothing here commands a peer.

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "dc_peer.h"

#define DC_REGISTRY_MAX 12   // Dragon families are small; evict the least-recently-seen

// A device counts as offline once nothing (announce or status) has been heard within
// this window. The console shows it greyed rather than dropping it immediately.
#define DC_REGISTRY_STALE_US  (30LL * 1000000LL)

typedef struct {
    char     id[DC_PEER_ID_MAX];        // peer_id from the frame header (table key)
    uint8_t  kind;                      // dc_peer_kind_t (UNKNOWN until an ANNOUNCE arrives)
    char     name[DC_PEER_NAME_MAX];    // friendly name from ANNOUNCE ("" until heard)
    char     fw[DC_PEER_FW_MAX];        // firmware version from ANNOUNCE
    uint8_t  ip[4];                     // STA IPv4 from ANNOUNCE (for later control)
    uint16_t caps;                      // capability bitmask from ANNOUNCE

    int64_t  last_seen_us;              // most recent frame of any kind (freshness)
    int64_t  last_announce_us;          // most recent ANNOUNCE (0 = never)
    int64_t  last_status_us;            // most recent status frame (0 = never)

    bool     has_status;                // a status frame has been stored
    uint8_t  status_cap;               // which dc_peer_cap_t the union below holds
    union {
        dc_peer_heater_t heater;        // DC_PEER_CAP_HEATER
        dc_peer_vent_t   vent;          // DC_PEER_CAP_VENT
        dc_peer_dryer_t  dryer;         // DC_PEER_CAP_DRYER
    } status;
} dc_registry_entry_t;

// Subscribe to ANNOUNCE + all status capabilities and begin populating the table.
// Call once, after dc_peer_start(). Idempotent.
esp_err_t dc_registry_start(void);

// Snapshot the table into `out` (up to `max` entries), most-recently-seen first.
// Returns the number written. Safe to call from any task.
int dc_registry_get(dc_registry_entry_t *out, int max);

// True if this entry has not been heard within DC_REGISTRY_STALE_US.
static inline bool dc_registry_entry_stale(const dc_registry_entry_t *e, int64_t now_us)
{
    return (now_us - e->last_seen_us) >= DC_REGISTRY_STALE_US;
}
