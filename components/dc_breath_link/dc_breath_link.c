// SPDX-License-Identifier: MIT
#include "dc_breath_link.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "nvs.h"
#include "cJSON.h"
#include "dc_peer.h"

#include <math.h>
#include <string.h>

static const char *TAG = "dc_breath_link";

#define NVS_NS      "app_nvs"
#define KEY_EN      "bl_en"       // u8: info source enabled
#define KEY_ADDR    "bl_addr"     // str: breath host/IP

#define HTTP_TIMEOUT_MS  3000
#define IDLE_POLL_MS     5000     // re-check config/active while idle
#define RESP_MAX         4096     // /api/v2/state is ~2-3 KB

static SemaphoreHandle_t   s_lock = NULL;
static TaskHandle_t        s_task = NULL;
static dc_breath_link_config_t s_cfg = {0};
static volatile bool       s_active = false;   // consumer needs data (e.g. AUTO mode)
static dc_breath_snapshot_t s_snap = { .valid = false, .chamber_c = NAN };
static int64_t             s_espnow_us = 0;    // monotonic time of last good ESP-NOW frame

// If an ESP-NOW push has landed this recently, skip the HTTP poll — ESP-NOW is the
// primary transport and HTTP is only the fallback when no peer frames are arriving.
#define ESPNOW_PREFER_US  (30LL * 1000000LL)

// Body buffer — only the single poll task touches it. Pull-based read loop (see dc_prusa).
static char s_resp[RESP_MAX];

// ---------- config persistence ----------

static esp_err_t nvs_load(dc_breath_link_config_t *out)
{
    memset(out, 0, sizeof(*out));
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NS, NVS_READONLY, &h);
    if (err != ESP_OK) return err;
    uint8_t en = 0;
    if (nvs_get_u8(h, KEY_EN, &en) == ESP_OK) out->enabled = (en != 0);
    size_t sz = sizeof(out->address);
    nvs_get_str(h, KEY_ADDR, out->address, &sz);   // absent => empty => not configured
    nvs_close(h);
    return ESP_OK;
}

static esp_err_t nvs_save(const dc_breath_link_config_t *cfg)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;
    err = nvs_set_u8(h, KEY_EN, cfg->enabled ? 1 : 0);
    if (err == ESP_OK) err = nvs_set_str(h, KEY_ADDR, cfg->address);
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    return err;
}

// ---------- HTTP poll ----------

// Parse one /api/v2/state body into s_snap under s_lock. A valid Breath body must carry a
// string `mode`; anything short of that (a wrong host answering 200 with other content)
// returns false so the caller leaves the snapshot to age out to stale rather than
// masquerading as a live Breath. The whole sample is staged in locals and committed once.
static bool parse_state(const char *json)
{
    cJSON *root = cJSON_Parse(json);
    if (!root) return false;
    cJSON *mode = cJSON_GetObjectItemCaseSensitive(root, "mode");
    if (!cJSON_IsString(mode)) { cJSON_Delete(root); return false; }

    float target = NAN, chamber = NAN;
    bool demand = false, fault = false, inhibited = false;
    uint32_t rev = 0;

    cJSON *t = cJSON_GetObjectItemCaseSensitive(root, "target");
    if (cJSON_IsObject(t)) {
        cJSON *eff = cJSON_GetObjectItemCaseSensitive(t, "effective_c");
        cJSON *req = cJSON_GetObjectItemCaseSensitive(t, "requested_c");
        if (cJSON_IsNumber(eff))      target = (float)eff->valuedouble;
        else if (cJSON_IsNumber(req)) target = (float)req->valuedouble;
    }
    cJSON *h = cJSON_GetObjectItemCaseSensitive(root, "heater");
    if (cJSON_IsObject(h)) {
        cJSON *d = cJSON_GetObjectItemCaseSensitive(h, "demand");
        if (cJSON_IsBool(d)) demand = cJSON_IsTrue(d);
    }
    cJSON *sf = cJSON_GetObjectItemCaseSensitive(root, "safety");
    if (cJSON_IsObject(sf)) {
        cJSON *fl = cJSON_GetObjectItemCaseSensitive(sf, "fault_latched");
        cJSON *in = cJSON_GetObjectItemCaseSensitive(sf, "inhibited");
        if (cJSON_IsBool(fl)) fault = cJSON_IsTrue(fl);
        if (cJSON_IsBool(in)) inhibited = cJSON_IsTrue(in);
    }
    cJSON *se = cJSON_GetObjectItemCaseSensitive(root, "sensors");
    if (cJSON_IsObject(se)) {
        cJSON *ch = cJSON_GetObjectItemCaseSensitive(se, "chamber");
        if (cJSON_IsObject(ch)) {
            cJSON *tc = cJSON_GetObjectItemCaseSensitive(ch, "temperature_c");
            if (cJSON_IsNumber(tc)) chamber = (float)tc->valuedouble;
        }
    }
    cJSON *sr = cJSON_GetObjectItemCaseSensitive(root, "state_revision");
    if (cJSON_IsNumber(sr)) rev = (uint32_t)sr->valuedouble;

    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_snap.valid = true;
    s_snap.connected = true;
    strncpy(s_snap.mode, mode->valuestring, sizeof(s_snap.mode) - 1);
    s_snap.mode[sizeof(s_snap.mode) - 1] = '\0';
    s_snap.target_c = isfinite(target) ? target : 0.0f;
    s_snap.chamber_c = chamber;               // may stay NaN (display only)
    s_snap.demand = demand;
    s_snap.fault = fault;
    s_snap.inhibited = inhibited;
    s_snap.state_revision = rev;
    s_snap.transport = DC_BREATH_TX_HTTP;
    s_snap.peer_id[0] = '\0';
    s_snap.updated_us = esp_timer_get_time();
    xSemaphoreGive(s_lock);
    cJSON_Delete(root);
    return true;
}

// A failed poll does NOT touch updated_us — the snapshot simply ages toward stale, so a
// single dropped request never flips the Breath to "unreachable" (the 150 s fresh-window
// debounces ~2-3 misses). We only note that the last attempt failed.
static void mark_unreachable(void)
{
    if (!s_lock) return;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_snap.connected = false;
    xSemaphoreGive(s_lock);
}

static void poll_once(esp_http_client_handle_t client, char *url, size_t url_sz)
{
    char addr[DC_BREATH_ADDR_MAX];
    xSemaphoreTake(s_lock, portMAX_DELAY);
    snprintf(addr, sizeof(addr), "%s", s_cfg.address);
    xSemaphoreGive(s_lock);

    snprintf(url, url_sz, "http://%s/api/v2/state", addr);
    s_resp[0] = '\0';

    esp_http_client_set_url(client, url);
    esp_http_client_set_method(client, HTTP_METHOD_GET);
    esp_http_client_set_header(client, "Accept", "application/json");

    esp_err_t err = esp_http_client_open(client, 0);   // 0 = no request body
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "open %s: %s", url, esp_err_to_name(err));
        mark_unreachable();
        return;
    }
    esp_http_client_fetch_headers(client);
    int status = esp_http_client_get_status_code(client);
    if (status != 200) {
        esp_http_client_close(client);
        ESP_LOGW(TAG, "poll HTTP %d", status);
        mark_unreachable();
        return;
    }
    int total = 0, r;
    while (total < RESP_MAX - 1 &&
           (r = esp_http_client_read(client, s_resp + total, RESP_MAX - 1 - total)) > 0)
        total += r;
    s_resp[total > 0 ? total : 0] = '\0';
    esp_http_client_close(client);

    if (total <= 0 || !parse_state(s_resp)) mark_unreachable();
}

static void poll_task(void *arg)
{
    (void)arg;
    esp_http_client_config_t hc = {
        .url = "http://127.0.0.1/api/v2/state",   // replaced per poll
        .timeout_ms = HTTP_TIMEOUT_MS,
        .keep_alive_enable = false,               // fresh connection each poll (60 s cadence)
    };
    esp_http_client_handle_t client = esp_http_client_init(&hc);
    if (!client) { ESP_LOGE(TAG, "http client init failed"); s_task = NULL; vTaskDelete(NULL); return; }
    static char url[128];
    for (;;) {
        bool go;
        xSemaphoreTake(s_lock, portMAX_DELAY);
        // HTTP is the fallback transport: poll only when enabled, addressed, active, and
        // ESP-NOW push frames aren't already keeping the snapshot fresh.
        bool espnow_fresh = s_espnow_us != 0 &&
                            (esp_timer_get_time() - s_espnow_us) < ESPNOW_PREFER_US;
        go = s_cfg.enabled && s_cfg.address[0] && s_active && !espnow_fresh;
        xSemaphoreGive(s_lock);
        if (go) {
            poll_once(client, url, sizeof(url));
            vTaskDelay(pdMS_TO_TICKS(DC_BREATH_POLL_MS));
        } else {
            vTaskDelay(pdMS_TO_TICKS(IDLE_POLL_MS));   // idle: cheap re-check of config/active/espnow
        }
    }
}

// ---------- ESP-NOW ingest (dc_peer) ----------

// Runs in the ESP-NOW recv context. Maps a peer heater frame into the same snapshot the
// HTTP poll fills, stamping updated_us with LOCAL receipt time (RFC 0004 freshness).
// v1 accepts any peer while enabled; peer_id identity filtering is a follow-up.
static void on_peer_heater(const char *peer_id, dc_peer_cap_t cap,
                           const void *payload, size_t len, void *ctx)
{
    (void)peer_id; (void)cap; (void)ctx;
    if (len < sizeof(dc_peer_heater_t) || !s_lock) return;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    bool enabled = s_cfg.enabled;
    xSemaphoreGive(s_lock);
    if (!enabled) return;

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
    s_espnow_us = s_snap.updated_us;
    xSemaphoreGive(s_lock);
}

// ---------- public API ----------

esp_err_t dc_breath_link_start(void)
{
    if (s_task) return ESP_ERR_INVALID_STATE;
    if (!s_lock) { s_lock = xSemaphoreCreateMutex(); if (!s_lock) return ESP_ERR_NO_MEM; }

    dc_breath_link_config_t cfg;
    nvs_load(&cfg);                              // absent => disabled/empty, still start the (idle) task
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_cfg = cfg;
    xSemaphoreGive(s_lock);
    ESP_LOGI(TAG, "start: enabled=%d address='%s'", cfg.enabled, cfg.address);

    // Subscribe to ESP-NOW heater pushes (the primary transport). Harmless if the
    // device never calls dc_peer_start() — no frames will arrive.
    dc_peer_subscribe(DC_PEER_CAP_HEATER, on_peer_heater, NULL);

    if (xTaskCreate(poll_task, "dc_breath", 6144, NULL, 4, &s_task) != pdPASS) {
        s_task = NULL;
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

esp_err_t dc_breath_link_set_config(const dc_breath_link_config_t *cfg)
{
    if (!cfg) return ESP_ERR_INVALID_ARG;
    esp_err_t err = nvs_save(cfg);
    if (err != ESP_OK) return err;
    if (s_lock) {
        xSemaphoreTake(s_lock, portMAX_DELAY);
        s_cfg = *cfg;
        if (!s_cfg.enabled) s_snap.valid = false;   // drop stale data when the source is disabled
        xSemaphoreGive(s_lock);
    }
    return ESP_OK;
}

esp_err_t dc_breath_link_get_config(dc_breath_link_config_t *out)
{
    if (!out) return ESP_ERR_INVALID_ARG;
    nvs_load(out);   // absent => zeroed (disabled + empty)
    return ESP_OK;
}

bool dc_breath_link_configured(void)
{
    // Enabled is enough: ESP-NOW needs no address; the address only enables the HTTP
    // fallback poll. So a Vent with the source toggled on is "configured" and will
    // consume peer pushes even without an address.
    bool c;
    if (!s_lock) return false;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    c = s_cfg.enabled;
    xSemaphoreGive(s_lock);
    return c;
}

void dc_breath_link_set_active(bool active)
{
    s_active = active;
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
    nvs_erase_key(h, KEY_ADDR);
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
