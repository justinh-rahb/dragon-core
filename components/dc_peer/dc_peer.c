// SPDX-License-Identifier: MIT
#include "dc_peer.h"

#include "esp_now.h"
#include "esp_wifi.h"
#include "esp_log.h"
#include "esp_timer.h"

#include <stdio.h>
#include <string.h>

static const char *TAG = "dc_peer";
static const uint8_t BCAST[6] = { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };

#define MAX_CAPS 8   // capability ids are small; index the table directly

static char     s_self_id[DC_PEER_ID_MAX];
static bool     s_started;
static uint32_t s_seq;
static struct { dc_peer_rx_cb_t cb; void *ctx; } s_subs[MAX_CAPS];
static dc_peer_stats_t s_stats;

// Runs in the ESP-NOW/Wi-Fi recv context — keep it fast. Validates the envelope and
// dispatches the payload to the capability's subscriber. Subscribers must not block.
static void recv_cb(const esp_now_recv_info_t *info, const uint8_t *data, int len)
{
    (void)info;
    if (len < (int)sizeof(dc_peer_hdr_t)) return;
    const dc_peer_hdr_t *h = (const dc_peer_hdr_t *)data;
    if (h->magic != DC_PEER_MAGIC || h->version != DC_PEER_VERSION) return;
    if ((int)(sizeof(dc_peer_hdr_t) + h->payload_len) > len) return;
    if (h->capability == 0 || h->capability >= MAX_CAPS) return;
    char pid[DC_PEER_ID_MAX];
    memcpy(pid, h->peer_id, DC_PEER_ID_MAX);
    pid[DC_PEER_ID_MAX - 1] = '\0';
    s_stats.rx_frames++;
    s_stats.last_rx_us = esp_timer_get_time();
    memcpy(s_stats.last_peer_id, pid, DC_PEER_ID_MAX);
    dc_peer_rx_cb_t cb = s_subs[h->capability].cb;
    if (!cb) return;
    cb(pid, (dc_peer_cap_t)h->capability, data + sizeof(dc_peer_hdr_t), h->payload_len,
       s_subs[h->capability].ctx);
}

esp_err_t dc_peer_start(const char *self_id)
{
    if (s_started) return ESP_OK;
    if (self_id) snprintf(s_self_id, sizeof(s_self_id), "%s", self_id);

    esp_err_t err = esp_now_init();
    if (err != ESP_OK) { ESP_LOGE(TAG, "esp_now_init: %s", esp_err_to_name(err)); return err; }

    // Broadcast peer on the current channel (both devices share the AP's channel).
    esp_now_peer_info_t peer = {0};
    memcpy(peer.peer_addr, BCAST, 6);
    peer.channel = 0;                 // 0 = current channel
    peer.ifidx   = WIFI_IF_STA;
    peer.encrypt = false;
    err = esp_now_add_peer(&peer);
    if (err != ESP_OK && err != ESP_ERR_ESPNOW_EXIST) {
        ESP_LOGE(TAG, "add broadcast peer: %s", esp_err_to_name(err));
        return err;
    }
    err = esp_now_register_recv_cb(recv_cb);
    if (err != ESP_OK) { ESP_LOGE(TAG, "register recv cb: %s", esp_err_to_name(err)); return err; }

    s_started = true;
    s_stats.started = true;
    ESP_LOGI(TAG, "up (self_id='%s')", s_self_id);
    return ESP_OK;
}

void dc_peer_get_stats(dc_peer_stats_t *out)
{
    if (out) *out = s_stats;
}

esp_err_t dc_peer_publish(dc_peer_cap_t cap, const void *payload, size_t len)
{
    if (!s_started) return ESP_ERR_INVALID_STATE;
    if (len > DC_PEER_PAYLOAD_MAX) return ESP_ERR_INVALID_ARG;

    uint8_t buf[sizeof(dc_peer_hdr_t) + DC_PEER_PAYLOAD_MAX];
    dc_peer_hdr_t *h = (dc_peer_hdr_t *)buf;
    h->magic       = DC_PEER_MAGIC;
    h->version     = DC_PEER_VERSION;
    h->capability  = (uint8_t)cap;
    h->payload_len = (uint8_t)len;
    h->seq         = ++s_seq;
    memset(h->peer_id, 0, DC_PEER_ID_MAX);
    memcpy(h->peer_id, s_self_id, strnlen(s_self_id, DC_PEER_ID_MAX - 1));
    if (payload && len) memcpy(buf + sizeof(dc_peer_hdr_t), payload, len);

    s_stats.tx_frames++;
    return esp_now_send(BCAST, buf, sizeof(dc_peer_hdr_t) + len);
}

esp_err_t dc_peer_subscribe(dc_peer_cap_t cap, dc_peer_rx_cb_t cb, void *ctx)
{
    if (cap == 0 || (int)cap >= MAX_CAPS) return ESP_ERR_INVALID_ARG;
    s_subs[cap].cb = cb;
    s_subs[cap].ctx = ctx;
    return ESP_OK;
}
