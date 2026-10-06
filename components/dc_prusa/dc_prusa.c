// SPDX-License-Identifier: MIT
#include "dc_prusa.h"
#include "dc_prusa_freshness.h"
#include "dc_prusa_material.h"
#include "dc_prusa_parse.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "nvs.h"
#include "cJSON.h"

#include <math.h>
#include <string.h>

static const char *TAG = "dc_prusa";

#define NVS_NS        "app_nvs"
#define KEY_HOST      "pr_host"
#define KEY_PORT      "pr_port"
#define KEY_APIKEY    "pr_key"

#define POLL_PERIOD_MS   5000         // socket-starved nhttp: 5 s comfortable
#define HTTP_TIMEOUT_MS  4000         // /api/v1/status (the freshness-critical poll)
// Material polling (/api/v1/job + /api/printer) runs AFTER the status stamp, so its
// wall-clock time delays the next status refresh and must not push the gap past the
// 15 s freshness window. Budget: 2 material requests + poll delay + next status fetch
// must stay under DC_PRUSA_STATUS_STALE_US. With a 2 s material timeout the worst case
// is 2*2 + 5 (POLL_PERIOD) + 4 (HTTP_TIMEOUT) = 13 s < 15 s, even if both material
// endpoints time out every cycle. A separate, tighter deadline than the status poll.
#define MATERIAL_TIMEOUT_MS 2000
#define RESP_MAX         1536         // /api/v1/status is ~300-400 B; generous
#define JOB_RESP_MAX     4096         // /api/v1/job carries file.meta (larger)
// Material token cap + per-tool max live in dc_prusa_parse.h (DC_PRUSA_MAT_MAX /
// DC_PRUSA_TOOLS_MAX) so the parsers and their host test share one definition.

static SemaphoreHandle_t s_lock = NULL;
static TaskHandle_t      s_task = NULL;
static dc_prusa_config_t s_cfg = {0};
static dc_prusa_status_t s_status = {
    .state = DC_PRUSA_DISABLED,
    .online = false,
    .bed_temp = NAN,
    .bed_target = 0.0f,
    .printer_state = "",
    .material = "",
    .status_age_ms = UINT32_MAX,
};
static int64_t s_status_us = 0;  // monotonic timestamp of last complete status sample

const char *dc_prusa_state_str(dc_prusa_state_t s)
{
    switch (s) {
    case DC_PRUSA_DISABLED:    return "disabled";
    case DC_PRUSA_CONNECTING:  return "connecting";
    case DC_PRUSA_ONLINE:      return "online";
    case DC_PRUSA_AUTH_FAILED: return "auth_failed";
    case DC_PRUSA_OFFLINE:     return "offline";
    default:                   return "?";
    }
}

// ---------- config persistence ----------

static void apply_defaults(dc_prusa_config_t *c)
{
    if (c->port == 0) c->port = DC_PRUSA_DEFAULT_PORT;
}

static esp_err_t nvs_load(dc_prusa_config_t *out)
{
    memset(out, 0, sizeof(*out));
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NS, NVS_READONLY, &h);
    if (err != ESP_OK) return err;
    size_t sz = sizeof(out->host);
    err = nvs_get_str(h, KEY_HOST, out->host, &sz);
    if (err != ESP_OK) { nvs_close(h); return err; }   // no host => not configured
    uint16_t p = 0;
    if (nvs_get_u16(h, KEY_PORT, &p) == ESP_OK && p > 0) out->port = p;
    sz = sizeof(out->api_key);
    nvs_get_str(h, KEY_APIKEY, out->api_key, &sz);      // optional
    nvs_close(h);
    apply_defaults(out);
    return ESP_OK;
}

static esp_err_t nvs_save(const dc_prusa_config_t *cfg)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;
    err = nvs_set_str(h, KEY_HOST, cfg->host);
    if (err == ESP_OK) err = nvs_set_u16(h, KEY_PORT, cfg->port ? cfg->port : DC_PRUSA_DEFAULT_PORT);
    if (err == ESP_OK) err = nvs_set_str(h, KEY_APIKEY, cfg->api_key);
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    return err;
}

// ---------- HTTP poll ----------

// Response body buffer — only the single poll task touches it. We read the body with
// the pull-based open/fetch_headers/read loop rather than an ON_DATA event handler
// (whose evt->user_data plumbing yielded a truncated/garbled body here).
static char s_resp[RESP_MAX];

// Parse printer.{temp_bed,target_bed,state} into s_status under s_lock. Returns true
// only if the body was valid PrusaLink JSON carrying a printer object — so a wrong
// host that answers 200 with non-JSON (e.g. another web UI's HTML) fails to OFFLINE
// rather than masquerading as a connected printer.
// Parse one /api/v1/status body into s_status. STRICT + ATOMIC: a valid live sample
// must carry printer.temp_bed AND printer.target_bed as finite numbers AND a non-empty
// printer.state string. Anything short of that (a partial body, a non-numeric field, a
// printer object missing target_bed) returns false so the caller fails to OFFLINE — it
// must NEVER leave a stale bed_target in place and keep the chamber heating. The whole
// sample is staged in locals and committed under the lock in one shot (no partial write).
static bool parse_status(const char *json)
{
    cJSON *root = cJSON_Parse(json);
    if (!root) return false;

    float bed_temp = NAN, bed_target = NAN;
    const char *state = NULL;
    cJSON *pr = cJSON_GetObjectItemCaseSensitive(root, "printer");
    if (cJSON_IsObject(pr)) {
        cJSON *tb = cJSON_GetObjectItemCaseSensitive(pr, "temp_bed");
        cJSON *gb = cJSON_GetObjectItemCaseSensitive(pr, "target_bed");
        cJSON *st = cJSON_GetObjectItemCaseSensitive(pr, "state");
        if (cJSON_IsNumber(tb)) bed_temp = (float)tb->valuedouble;
        if (cJSON_IsNumber(gb)) bed_target = (float)gb->valuedouble;
        if (cJSON_IsString(st)) state = st->valuestring;
    }

    bool ok = isfinite(bed_temp) && isfinite(bed_target) && state && state[0];
    if (ok) {
        xSemaphoreTake(s_lock, portMAX_DELAY);
        s_status.bed_temp = bed_temp;
        s_status.bed_target = bed_target;
        strncpy(s_status.printer_state, state, sizeof(s_status.printer_state) - 1);
        s_status.printer_state[sizeof(s_status.printer_state) - 1] = '\0';
        s_status_us = esp_timer_get_time();
        s_status.status_age_ms = 0;
        xSemaphoreGive(s_lock);
    }
    cJSON_Delete(root);
    return ok;
}

// Response buffer for /api/v1/job (separate from s_resp: the job body carries
// file.meta and is larger). Only the poll task touches it.
static char s_job[JOB_RESP_MAX];

// True for printer states where a filament is loaded and being laid down, so the
// job's material is meaningful. PrusaLink reports PRINTING/PAUSED during a job.
static bool state_is_printing(const char *state)
{
    return state && (strcmp(state, "PRINTING") == 0 || strcmp(state, "PAUSED") == 0);
}

// The /api/v1/job and /api/printer material parsers are pure and live in
// dc_prusa_parse.h (dc_prusa_parse_job_material / dc_prusa_parse_printer_material)
// so they can be host-tested against real payloads. See tests/.

// Commit (or clear) the printing material under the lock, without disturbing the
// status freshness timestamp — material rides the same sample as the bed data.
static void commit_material(const char *material)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (material && material[0]) {
        strncpy(s_status.material, material, sizeof(s_status.material) - 1);
        s_status.material[sizeof(s_status.material) - 1] = '\0';
    } else {
        s_status.material[0] = '\0';
    }
    xSemaphoreGive(s_lock);
}

static void set_state(dc_prusa_state_t st)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (s_status.state != st)
        ESP_LOGI(TAG, "%s -> %s", dc_prusa_state_str(s_status.state), dc_prusa_state_str(st));
    s_status.state = st;
    s_status.online = (st == DC_PRUSA_ONLINE);
    if (st != DC_PRUSA_ONLINE) {
        s_status.bed_target = 0.0f;      // fail-safe: don't hold a stale follow
        s_status.material[0] = '\0';     // and don't hold a stale filament zone
    }
    xSemaphoreGive(s_lock);
}

// GET `path` on the configured host into `buf`. Returns bytes read (>= 0) on HTTP
// 200, or -1 on open failure / non-200 (e.g. 204 no-job). Sets auth + accept. Used
// only for the best-effort MATERIAL requests, so it runs on the tighter
// MATERIAL_TIMEOUT_MS deadline to protect status freshness (see the macro).
static int http_get(esp_http_client_handle_t client, char *url, size_t url_sz,
                    const dc_prusa_config_t *cfg, const char *path,
                    char *buf, size_t buf_sz)
{
    snprintf(url, url_sz, "http://%s:%u%s", cfg->host, (unsigned)cfg->port, path);
    buf[0] = '\0';
    esp_http_client_set_timeout_ms(client, MATERIAL_TIMEOUT_MS);
    esp_http_client_set_url(client, url);
    esp_http_client_set_method(client, HTTP_METHOD_GET);
    esp_http_client_set_header(client, "X-Api-Key", cfg->api_key);
    esp_http_client_set_header(client, "Accept", "application/json");
    if (esp_http_client_open(client, 0) != ESP_OK) return -1;
    esp_http_client_fetch_headers(client);
    if (esp_http_client_get_status_code(client) != 200) {
        esp_http_client_close(client);
        return -1;
    }
    int total = 0, r;
    while (total < (int)buf_sz - 1 &&
           (r = esp_http_client_read(client, buf + total, (int)buf_sz - 1 - total)) > 0)
        total += r;
    buf[total > 0 ? total : 0] = '\0';
    esp_http_client_close(client);
    return total;
}

// Second poll step: resolve the printing filament, but only while a print is
// active. Prefer the modern v1 /api/v1/job file.meta.filament_type (present for
// PrusaLink-parsed files); when that is absent — e.g. a file sent onto USB, which
// PrusaLink does not parse — fall back to the legacy /api/printer
// telemetry.material (the only reliable signal in that case). Clears the material
// (-> AUTO idle) when neither reports one. Reuses the status poll's client + url.
static void poll_job(esp_http_client_handle_t client, char *url, size_t url_sz,
                     const dc_prusa_config_t *cfg)
{
    char state[sizeof(s_status.printer_state)];
    xSemaphoreTake(s_lock, portMAX_DELAY);
    strncpy(state, s_status.printer_state, sizeof(state) - 1);
    state[sizeof(state) - 1] = '\0';
    xSemaphoreGive(s_lock);
    if (!state_is_printing(state)) { commit_material(NULL); return; }   // idle: no material

    char material[DC_PRUSA_MAT_MAX];

    // 1) Modern v1 job metadata.
    int n = http_get(client, url, url_sz, cfg, "/api/v1/job", s_job, JOB_RESP_MAX);
    if (n > 0 && dc_prusa_parse_job_material(s_job, material, sizeof(material))) {
        commit_material(material);
        return;
    }

    // 2) Legacy telemetry.material fallback (loaded filament, always present).
    n = http_get(client, url, url_sz, cfg, "/api/printer", s_job, JOB_RESP_MAX);
    if (n > 0 && dc_prusa_parse_printer_material(s_job, material, sizeof(material))) {
        commit_material(material);
        return;
    }

    commit_material(NULL);   // neither endpoint reported a filament -> no material (AUTO idle)
}

static void poll_once(esp_http_client_handle_t client, char *url, size_t url_sz)
{
    dc_prusa_config_t cfg;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    cfg = s_cfg;
    xSemaphoreGive(s_lock);

    snprintf(url, url_sz, "http://%s:%u/api/v1/status", cfg.host, (unsigned)cfg.port);
    s_resp[0] = '\0';

    // Restore the full status deadline — http_get() (material) lowers the shared
    // client to MATERIAL_TIMEOUT_MS, and status is the freshness-critical poll.
    esp_http_client_set_timeout_ms(client, HTTP_TIMEOUT_MS);
    esp_http_client_set_url(client, url);
    esp_http_client_set_method(client, HTTP_METHOD_GET);
    esp_http_client_set_header(client, "X-Api-Key", cfg.api_key);
    esp_http_client_set_header(client, "Accept", "application/json");

    esp_err_t err = esp_http_client_open(client, 0);   // 0 = no request body
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "open %s: %s", url, esp_err_to_name(err));
        set_state(DC_PRUSA_OFFLINE);
        return;
    }
    esp_http_client_fetch_headers(client);
    int status = esp_http_client_get_status_code(client);
    if (status == 401) { esp_http_client_close(client); set_state(DC_PRUSA_AUTH_FAILED); return; }
    if (status != 200)  { esp_http_client_close(client); ESP_LOGW(TAG, "poll HTTP %d", status);
                          set_state(DC_PRUSA_OFFLINE); return; }
    int total = 0, r;
    while (total < RESP_MAX - 1 &&
           (r = esp_http_client_read(client, s_resp + total, RESP_MAX - 1 - total)) > 0)
        total += r;
    s_resp[total > 0 ? total : 0] = '\0';
    esp_http_client_close(client);
    // Fail safe to OFFLINE unless the body parsed as PrusaLink JSON — a wrong host that
    // answers 200 with non-JSON must never look like a connected printer.
    if (total <= 0 || !parse_status(s_resp)) { set_state(DC_PRUSA_OFFLINE); return; }
    set_state(DC_PRUSA_ONLINE);
    // Material detection is a SEPARATE, best-effort step run AFTER the source is
    // already ONLINE. It only ever writes s_status.material (never state, bed data,
    // or the freshness timestamp), and it runs on a tighter MATERIAL_TIMEOUT_MS
    // deadline so it cannot delay the next status poll past the freshness window.
    // So a missing/renamed/removed job-or-printer endpoint, an unparseable body, or
    // a future PrusaLink that drops the legacy API simply yields no material (AUTO
    // goes idle) — the source stays online and connected. Forward-compatible by design.
    poll_job(client, url, url_sz, &cfg);
}

static void poll_task(void *arg)
{
    (void)arg;
    esp_http_client_config_t hc = {
        .url = "http://127.0.0.1/api/v1/status",   // replaced per poll
        .timeout_ms = HTTP_TIMEOUT_MS,
        .keep_alive_enable = false,                // fresh connection each poll (5s cadence)
    };
    esp_http_client_handle_t client = esp_http_client_init(&hc);
    if (!client) { ESP_LOGE(TAG, "http client init failed"); vTaskDelete(NULL); return; }
    static char url[128];
    for (;;) {
        poll_once(client, url, sizeof(url));
        vTaskDelay(pdMS_TO_TICKS(POLL_PERIOD_MS));
    }
}

// ---------- public API ----------

esp_err_t dc_prusa_start(void)
{
    if (s_task) return ESP_ERR_INVALID_STATE;
    if (!s_lock) { s_lock = xSemaphoreCreateMutex(); if (!s_lock) return ESP_ERR_NO_MEM; }

    dc_prusa_config_t cfg;
    if (nvs_load(&cfg) != ESP_OK || cfg.host[0] == '\0') {
        ESP_LOGI(TAG, "no PrusaLink host configured; idle");
        set_state(DC_PRUSA_DISABLED);
        return ESP_OK;   // nothing to poll, not an error
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_cfg = cfg;
    xSemaphoreGive(s_lock);
    set_state(DC_PRUSA_CONNECTING);
    ESP_LOGI(TAG, "polling http://%s:%u/api/v1/status every %d ms",
             cfg.host, (unsigned)cfg.port, POLL_PERIOD_MS);
    if (xTaskCreate(poll_task, "dc_prusa", 5120, NULL, 4, &s_task) != pdPASS) {
        s_task = NULL;
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

esp_err_t dc_prusa_set_config(const dc_prusa_config_t *cfg)
{
    if (!cfg) return ESP_ERR_INVALID_ARG;
    dc_prusa_config_t c = *cfg;
    apply_defaults(&c);
    esp_err_t err = nvs_save(&c);
    if (err != ESP_OK) return err;
    if (s_lock) { xSemaphoreTake(s_lock, portMAX_DELAY); s_cfg = c; xSemaphoreGive(s_lock); }
    return ESP_OK;
}

esp_err_t dc_prusa_get_config(dc_prusa_config_t *out)
{
    if (!out) return ESP_ERR_INVALID_ARG;
    esp_err_t err = nvs_load(out);
    if (err != ESP_OK) {          // unconfigured: return blank + defaults
        memset(out, 0, sizeof(*out));
        apply_defaults(out);
        return ESP_OK;
    }
    return ESP_OK;
}

esp_err_t dc_prusa_get_status(dc_prusa_status_t *out)
{
    if (!out) return ESP_ERR_INVALID_ARG;

    int64_t sample_us = 0;
    if (!s_lock) {
        *out = s_status;
    } else {
        xSemaphoreTake(s_lock, portMAX_DELAY);
        *out = s_status;
        sample_us = s_status_us;
        xSemaphoreGive(s_lock);
    }

    dc_prusa_status_apply_freshness(out, esp_timer_get_time(), sample_us);
    return ESP_OK;
}

esp_err_t dc_prusa_clear_config(void)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;
    nvs_erase_key(h, KEY_HOST);
    nvs_erase_key(h, KEY_PORT);
    nvs_erase_key(h, KEY_APIKEY);
    err = nvs_commit(h);
    nvs_close(h);
    return err;
}
