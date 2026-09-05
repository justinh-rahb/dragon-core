// SPDX-License-Identifier: MIT
#include "dc_registry.h"

#include "esp_timer.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"

#include <string.h>

static const char *TAG = "dc_registry";

static dc_registry_entry_t s_tbl[DC_REGISTRY_MAX];
static int                 s_n;
static portMUX_TYPE        s_mux = portMUX_INITIALIZER_UNLOCKED;

// Find (or make) the entry for `id`. Called inside the critical section. Evicts the
// least-recently-seen entry when full. Returns NULL only if `id` is empty.
static dc_registry_entry_t *entry_for(const char *id, int64_t now)
{
    if (!id || !id[0]) return NULL;
    for (int i = 0; i < s_n; i++)
        if (strncmp(s_tbl[i].id, id, DC_PEER_ID_MAX) == 0) return &s_tbl[i];

    int slot;
    if (s_n < DC_REGISTRY_MAX) {
        slot = s_n++;
    } else {                        // evict least-recently-seen
        slot = 0;
        for (int i = 1; i < DC_REGISTRY_MAX; i++)
            if (s_tbl[i].last_seen_us < s_tbl[slot].last_seen_us) slot = i;
    }
    memset(&s_tbl[slot], 0, sizeof(s_tbl[slot]));
    strncpy(s_tbl[slot].id, id, DC_PEER_ID_MAX - 1);
    s_tbl[slot].id[DC_PEER_ID_MAX - 1] = '\0';
    (void)now;
    return &s_tbl[slot];
}

// Runs in the ESP-NOW recv context — keep it short. dc_peer has already validated the
// envelope and matched the payload length for the capability.
static void on_frame(const char *peer_id, dc_peer_cap_t cap,
                     const void *payload, size_t len, void *ctx)
{
    (void)ctx;
    // Clamp discovery to the known Dragon family: ignore any sender whose id isn't a
    // "dragon<kind>-" id, so other ESP-NOW traffic that speaks our envelope never shows
    // up as a device. (A name filter, not authentication — see dc_peer_kind_from_id.)
    if (dc_peer_kind_from_id(peer_id) == DC_PEER_KIND_UNKNOWN) return;
    int64_t now = esp_timer_get_time();
    portENTER_CRITICAL(&s_mux);
    dc_registry_entry_t *e = entry_for(peer_id, now);
    if (e) {
        e->last_seen_us = now;
        if (cap == DC_PEER_CAP_ANNOUNCE) {
            if (len >= sizeof(dc_peer_announce_t)) {
                const dc_peer_announce_t *a = (const dc_peer_announce_t *)payload;
                e->kind = a->kind;
                e->caps = a->caps;
                memcpy(e->ip, a->ip, 4);
                memcpy(e->name, a->name, DC_PEER_NAME_MAX);
                e->name[DC_PEER_NAME_MAX - 1] = '\0';
                memcpy(e->fw, a->fw, DC_PEER_FW_MAX);
                e->fw[DC_PEER_FW_MAX - 1] = '\0';
                e->last_announce_us = now;
            }
        } else {
            // A status capability: store the latest blob, tagged by cap.
            size_t n = 0;
            switch (cap) {
                case DC_PEER_CAP_HEATER: n = sizeof(dc_peer_heater_t); break;
                case DC_PEER_CAP_VENT:   n = sizeof(dc_peer_vent_t);   break;
                case DC_PEER_CAP_DRYER:  n = sizeof(dc_peer_dryer_t);  break;
                default: break;
            }
            if (n && len >= n) {
                memcpy(&e->status, payload, n);
                e->status_cap = (uint8_t)cap;
                e->has_status = true;
                e->last_status_us = now;
                if (e->kind == DC_PEER_KIND_UNKNOWN) {
                    // Infer a provisional kind before the first ANNOUNCE arrives.
                    e->kind = cap == DC_PEER_CAP_HEATER ? DC_PEER_KIND_BREATH
                            : cap == DC_PEER_CAP_VENT   ? DC_PEER_KIND_VENT
                            : cap == DC_PEER_CAP_DRYER  ? DC_PEER_KIND_WHEEZE
                                                        : DC_PEER_KIND_UNKNOWN;
                }
            }
        }
    }
    portEXIT_CRITICAL(&s_mux);
}

esp_err_t dc_registry_start(void)
{
    esp_err_t err = dc_peer_subscribe(DC_PEER_CAP_ANNOUNCE, on_frame, NULL);
    if (err == ESP_OK) err = dc_peer_subscribe(DC_PEER_CAP_HEATER, on_frame, NULL);
    if (err == ESP_OK) err = dc_peer_subscribe(DC_PEER_CAP_VENT,   on_frame, NULL);
    if (err == ESP_OK) err = dc_peer_subscribe(DC_PEER_CAP_DRYER,  on_frame, NULL);
    if (err == ESP_OK) ESP_LOGI(TAG, "device registry up (announce + heater/vent/dryer)");
    return err;
}

int dc_registry_get(dc_registry_entry_t *out, int max)
{
    if (!out || max <= 0) return 0;
    portENTER_CRITICAL(&s_mux);
    int n = s_n < max ? s_n : max;
    for (int i = 0; i < n; i++) out[i] = s_tbl[i];
    portEXIT_CRITICAL(&s_mux);
    // Stable alphabetical order by id. Sorting by recency made the list reshuffle on
    // every frame; a console wants a list that holds still. Small n — insertion sort.
    for (int i = 1; i < n; i++) {
        dc_registry_entry_t t = out[i];
        int j = i - 1;
        while (j >= 0 && strcmp(out[j].id, t.id) > 0) { out[j + 1] = out[j]; j--; }
        out[j + 1] = t;
    }
    return n;
}
