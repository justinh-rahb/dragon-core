// SPDX-License-Identifier: MIT
#include "dc_breath_link.h"

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "nvs.h"
#include "dc_peer.h"

#include <math.h>
#include <string.h>

static const char *TAG = "dc_breath_link";

#define NVS_NS   "app_nvs"
#define KEY_EN   "bl_en"     // u8: info source enabled
#define KEY_PEER "bl_peer"   // str: bound DragonBreath peer id ("" = unbound)

static SemaphoreHandle_t       s_lock = NULL;
static dc_breath_link_config_t s_cfg = {0};
static dc_breath_snapshot_t    s_snap = { .valid = false, .chamber_c = NAN };

// ---------- config persistence ----------

static esp_err_t nvs_load(dc_breath_link_config_t *out)
{
    memset(out, 0, sizeof(*out));
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NS, NVS_READONLY, &h);
    if (err != ESP_OK) return err;
    uint8_t en = 0;
    if (nvs_get_u8(h, KEY_EN, &en) == ESP_OK) out->enabled = (en != 0);
    size_t sz = sizeof(out->peer_id);
    nvs_get_str(h, KEY_PEER, out->peer_id, &sz);   // absent => empty => unbound (no signal)
    nvs_close(h);
    return ESP_OK;
}

static esp_err_t nvs_save(const dc_breath_link_config_t *cfg)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;
    err = nvs_set_u8(h, KEY_EN, cfg->enabled ? 1 : 0);
    if (err == ESP_OK) err = nvs_set_str(h, KEY_PEER, cfg->peer_id);
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    return err;
}

// ---------- ESP-NOW ingest (dc_peer) ----------

// Runs in the ESP-NOW recv context. Maps a peer heater frame into the snapshot with
// LOCAL receipt time (RFC 0004 freshness). Accepts a frame only when enabled and from the
// one explicitly bound peer (no "accept any").
static void on_peer_heater(const char *peer_id, dc_peer_cap_t cap,
                           const void *payload, size_t len, void *ctx)
{
    (void)cap; (void)ctx;
    if (len < sizeof(dc_peer_heater_t) || !s_lock) return;

    xSemaphoreTake(s_lock, portMAX_DELAY);
    // Require an explicit bound peer and an exact match. An unset peer_id accepts
    // NOTHING (not "any"): the vent acts on this heater state, so it must be tied to
    // one chosen Breath rather than adopting whoever broadcasts first.
    bool ok = s_cfg.enabled && s_cfg.peer_id[0] != '\0' &&
              strncmp(s_cfg.peer_id, peer_id, DC_PEER_ID_MAX) == 0;
    xSemaphoreGive(s_lock);
    if (!ok) return;

    const dc_peer_heater_t *hp = (const dc_peer_heater_t *)payload;
    const char *mode = hp->mode == 1 ? "power_on"
                     : hp->mode == 2 ? "auto"
                     : hp->mode == 3 ? "drying" : "off";
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_snap.valid = true;
    s_snap.connected = true;
    strncpy(s_snap.mode, mode, sizeof(s_snap.mode) - 1);
    s_snap.mode[sizeof(s_snap.mode) - 1] = '\0';
    s_snap.target_c  = (float)hp->target_dc / 10.0f;
    s_snap.chamber_c = hp->chamber_dc == DC_PEER_TEMP_UNKNOWN ? NAN : (float)hp->chamber_dc / 10.0f;
    s_snap.demand    = (hp->flags & DC_PEER_HEATER_DEMAND) != 0;
    s_snap.fault     = (hp->flags & DC_PEER_HEATER_FAULT) != 0;
    s_snap.inhibited = (hp->flags & DC_PEER_HEATER_INHIBITED) != 0;
    s_snap.state_revision = hp->state_revision;
    s_snap.transport = DC_BREATH_TX_ESPNOW;
    strncpy(s_snap.peer_id, peer_id, sizeof(s_snap.peer_id) - 1);
    s_snap.peer_id[sizeof(s_snap.peer_id) - 1] = '\0';
    s_snap.updated_us = esp_timer_get_time();
    xSemaphoreGive(s_lock);
}

// ---------- public API ----------

esp_err_t dc_breath_link_start(void)
{
    if (!s_lock) { s_lock = xSemaphoreCreateMutex(); if (!s_lock) return ESP_ERR_NO_MEM; }
    dc_breath_link_config_t cfg;
    nvs_load(&cfg);
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_cfg = cfg;
    xSemaphoreGive(s_lock);
    ESP_LOGI(TAG, "start: enabled=%d peer='%s'", cfg.enabled, cfg.peer_id);
    return dc_peer_subscribe(DC_PEER_CAP_HEATER, on_peer_heater, NULL);
}

esp_err_t dc_breath_link_set_config(const dc_breath_link_config_t *cfg)
{
    if (!cfg) return ESP_ERR_INVALID_ARG;
    esp_err_t err = nvs_save(cfg);
    if (err != ESP_OK) return err;
    if (s_lock) {
        xSemaphoreTake(s_lock, portMAX_DELAY);
        bool peer_changed = strncmp(s_cfg.peer_id, cfg->peer_id, DC_PEER_ID_MAX) != 0;
        s_cfg = *cfg;
        // Drop stale data when disabled or when the bound peer changes.
        if (!s_cfg.enabled || peer_changed) s_snap.valid = false;
        xSemaphoreGive(s_lock);
    }
    return ESP_OK;
}

esp_err_t dc_breath_link_get_config(dc_breath_link_config_t *out)
{
    if (!out) return ESP_ERR_INVALID_ARG;
    nvs_load(out);
    return ESP_OK;
}

bool dc_breath_link_configured(void)
{
    bool c;
    if (!s_lock) return false;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    c = s_cfg.enabled;
    xSemaphoreGive(s_lock);
    return c;
}

bool dc_breath_link_get(dc_breath_snapshot_t *out)
{
    if (!out) return false;
    if (!s_lock) { memset(out, 0, sizeof(*out)); out->chamber_c = NAN; return false; }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    *out = s_snap;
    xSemaphoreGive(s_lock);
    return out->valid;
}

bool dc_breath_link_heater_running(void)
{
    dc_breath_snapshot_t s;
    if (!dc_breath_link_configured()) return false;
    if (!dc_breath_link_get(&s) || !s.valid) return false;
    if ((esp_timer_get_time() - s.updated_us) >= DC_BREATH_FRESH_US) return false;   // stale
    if (s.fault || s.inhibited || !(s.target_c > 0.0f)) return false;
    return strcmp(s.mode, "power_on") == 0 ||
           strcmp(s.mode, "auto") == 0 ||
           strcmp(s.mode, "drying") == 0;
}

esp_err_t dc_breath_link_clear_config(void)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;
    nvs_erase_key(h, KEY_EN);
    nvs_erase_key(h, KEY_PEER);
    err = nvs_commit(h);
    nvs_close(h);
    if (s_lock) {
        xSemaphoreTake(s_lock, portMAX_DELAY);
        memset(&s_cfg, 0, sizeof(s_cfg));
        s_snap.valid = false;
        xSemaphoreGive(s_lock);
    }
    return err;
}
