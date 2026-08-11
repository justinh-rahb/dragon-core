#include "dc_moonraker.h"
#include "dc_evlog.h"

#include "cJSON.h"
#include "esp_log.h"
#include "esp_websocket_client.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "nvs.h"

#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

static const char *TAG = "dc_moonraker";

#define NVS_NS       "app_nvs"
#define KEY_HOST     "mk_host"
#define KEY_PORT     "mk_port"
#define KEY_APIKEY   "mk_apikey"

#define DEFAULT_PORT           7125
#define SUBSCRIBE_ID           1
#define RX_BUF_BYTES           4096
#define NETWORK_TIMEOUT_MS     10000
#define RECONNECT_TIMEOUT_MS   5000

// Progress below this counts as "PREPARING" rather than "PRINTING", matching
// stock's Bambu "PREPARE" state (downloading / slicing / warming up).
#define PREPARING_PROGRESS_MAX 0.01f

static SemaphoreHandle_t         s_lock  = NULL;
static esp_websocket_client_handle_t s_ws = NULL;
static dc_moonraker_config_t     s_cfg   = {0};
static dc_moonraker_status_t     s_status = {
    .state = DC_MK_DISABLED,
    .chamber_temp = NAN,
};
static char                     *s_rx_buf = NULL;
static size_t                    s_rx_off = 0;

// Latest raw Klipper values seen so we can re-derive the six-state model
// whenever any of them changes.
static char  s_ps_state[16]     = "";   // print_stats.state
static char  s_wh_state[16]     = "";   // webhooks.state
static float s_last_progress    = 0.0f;

// Material sources, in priority order: an explicit save_variables value (user
// opt-in) wins; otherwise the slicer's per-tool filament_type list from the
// g-code metadata, indexed by the active tool (toolhead.extruder) so a
// multi-material printer reports the filament that's actually printing.
static char  s_mat_sv[16]       = "";      // save_variables.variables.material
static char  s_fil_list[8][12]  = {{0}};   // per-tool filament_type from metadata
static int   s_fil_count        = 0;
static int   s_active_tool      = 0;       // toolhead.extruder index (extruder=0, extruder1=1, ...)
static char  s_meta_file[64]    = "";      // filename we last requested metadata for

static void send_metadata_request(const char *filename);
static void maybe_fetch_metadata(void);

const char *dc_printer_state_str(dc_printer_state_t s)
{
    switch (s) {
    case DC_PRINTER_IDLE:      return "idle";
    case DC_PRINTER_PREPARING: return "preparing";
    case DC_PRINTER_PRINTING:  return "printing";
    case DC_PRINTER_PAUSED:    return "paused";
    case DC_PRINTER_COMPLETE:  return "complete";
    case DC_PRINTER_ERROR:     return "error";
    default:                   return "unknown";
    }
}

// ---------- NVS ----------

static esp_err_t nvs_load(dc_moonraker_config_t *out)
{
    memset(out, 0, sizeof(*out));
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NS, NVS_READONLY, &h);
    if (err != ESP_OK) return err;

    size_t sz = sizeof(out->host);
    err = nvs_get_str(h, KEY_HOST, out->host, &sz);
    if (err != ESP_OK) { nvs_close(h); return err; }

    uint16_t p = 0;
    if (nvs_get_u16(h, KEY_PORT, &p) == ESP_OK && p > 0) out->port = p;
    else out->port = DEFAULT_PORT;

    sz = sizeof(out->api_key);
    nvs_get_str(h, KEY_APIKEY, out->api_key, &sz);   // optional
    nvs_close(h);
    return ESP_OK;
}

static esp_err_t nvs_save(const dc_moonraker_config_t *cfg)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;
    err = nvs_set_str(h, KEY_HOST, cfg->host);
    if (err == ESP_OK) err = nvs_set_u16(h, KEY_PORT, cfg->port ? cfg->port : DEFAULT_PORT);
    if (err == ESP_OK) err = nvs_set_str(h, KEY_APIKEY, cfg->api_key);
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    return err;
}

// ---------- state derivation ----------

// Fold the latest print_stats.state + webhooks.state + progress into our
// six-state enum. Called under s_lock every time any of them changes.
//
// Rules (see docs/ROADMAP.md Phase 2):
//   webhooks.state != "ready"          -> ERROR (Klipper down / shutdown)
//   print_stats.state                  -> as follows
//     "printing", progress < 1%        -> PREPARING
//     "printing"                       -> PRINTING
//     "paused"                         -> PAUSED
//     "complete"                       -> COMPLETE
//     "error" / "cancelled"            -> ERROR
//     "standby" / ""                   -> IDLE
static void recompute_printer_state(void)
{
    dc_printer_state_t st = DC_PRINTER_UNKNOWN;

    if (s_wh_state[0] && strcmp(s_wh_state, "ready") != 0) {
        st = DC_PRINTER_ERROR;
    } else if (s_ps_state[0] == '\0' || strcmp(s_ps_state, "standby") == 0) {
        st = DC_PRINTER_IDLE;
    } else if (strcmp(s_ps_state, "printing") == 0) {
        st = (s_last_progress < PREPARING_PROGRESS_MAX)
                 ? DC_PRINTER_PREPARING
                 : DC_PRINTER_PRINTING;
    } else if (strcmp(s_ps_state, "paused") == 0) {
        st = DC_PRINTER_PAUSED;
    } else if (strcmp(s_ps_state, "complete") == 0) {
        st = DC_PRINTER_COMPLETE;
    } else if (strcmp(s_ps_state, "error") == 0 ||
               strcmp(s_ps_state, "cancelled") == 0) {
        st = DC_PRINTER_ERROR;
    } else {
        st = DC_PRINTER_IDLE;   // unknown Klipper state, treat as idle
    }

    if (st != s_status.printer) {
        ESP_LOGI(TAG, "printer state: %s -> %s",
                 dc_printer_state_str(s_status.printer),
                 dc_printer_state_str(st));
        dc_evlog_add("printer: %s -> %s",
                     dc_printer_state_str(s_status.printer),
                     dc_printer_state_str(st));
        s_status.printer = st;
    }
    s_status.printing = (st == DC_PRINTER_PRINTING);
}

// ---------- status parsing ----------

// Copy a JSON string field into `dst` (uppercase) if present.
static void copy_upper(char *dst, size_t dst_sz, const char *src)
{
    // Guard the empty buffer: dst_sz-1 is unsigned, so a 0 size would wrap to
    // SIZE_MAX and defeat the bound. No caller passes 0 today; this makes it safe
    // regardless (and there is no room to write even a NUL).
    if (dst_sz == 0) return;
    size_t i = 0;
    // Bounds check first so the destination index is proven in range before the
    // source byte is read (behaviour-identical: src is NUL-terminated).
    while (i < dst_sz - 1 && src[i]) {
        dst[i] = (char)toupper((unsigned char)src[i]);
        ++i;
    }
    dst[i] = '\0';
}

// Publish the effective material: explicit save_variables wins; else the active
// tool's filament from the metadata list (clamped, so a single-tool list or an
// out-of-range tool still resolves). Caller holds s_lock.
static void recompute_material(void)
{
    const char *src = "";
    if (s_mat_sv[0]) {
        src = s_mat_sv;
    } else if (s_fil_count > 0) {
        int idx = s_active_tool;
        if (idx < 0) idx = 0;
        if (idx >= s_fil_count) idx = s_fil_count - 1;
        src = s_fil_list[idx];
    }
    strncpy(s_status.material, src, sizeof(s_status.material) - 1);
    s_status.material[sizeof(s_status.material) - 1] = '\0';
}

// Merge one status object (e.g. {"heater_bed":{"temperature":55.3}}) into the
// cached status. Fields not present in the update are left untouched, matching
// Moonraker's delta semantics.
static void merge_status_object(cJSON *status)
{
    if (!cJSON_IsObject(status)) return;

    bool state_dirty = false;

    cJSON *print = cJSON_GetObjectItemCaseSensitive(status, "print_stats");
    if (cJSON_IsObject(print)) {
        cJSON *ps = cJSON_GetObjectItemCaseSensitive(print, "state");
        if (cJSON_IsString(ps)) {
            strncpy(s_ps_state, ps->valuestring, sizeof(s_ps_state) - 1);
            s_ps_state[sizeof(s_ps_state) - 1] = '\0';
            state_dirty = true;
        }
        cJSON *fn = cJSON_GetObjectItemCaseSensitive(print, "filename");
        if (cJSON_IsString(fn)) {
            strncpy(s_status.filename, fn->valuestring, sizeof(s_status.filename) - 1);
            s_status.filename[sizeof(s_status.filename) - 1] = '\0';
        }
    }

    cJSON *wh = cJSON_GetObjectItemCaseSensitive(status, "webhooks");
    if (cJSON_IsObject(wh)) {
        cJSON *ws = cJSON_GetObjectItemCaseSensitive(wh, "state");
        if (cJSON_IsString(ws)) {
            strncpy(s_wh_state, ws->valuestring, sizeof(s_wh_state) - 1);
            s_wh_state[sizeof(s_wh_state) - 1] = '\0';
            state_dirty = true;
        }
    }

    cJSON *vsd = cJSON_GetObjectItemCaseSensitive(status, "virtual_sdcard");
    if (cJSON_IsObject(vsd)) {
        cJSON *p = cJSON_GetObjectItemCaseSensitive(vsd, "progress");
        if (cJSON_IsNumber(p)) {
            s_last_progress = (float)p->valuedouble;
            s_status.progress = s_last_progress;
            state_dirty = true;   // affects PREPARING vs PRINTING
        }
    }

    cJSON *bed = cJSON_GetObjectItemCaseSensitive(status, "heater_bed");
    if (cJSON_IsObject(bed)) {
        cJSON *t = cJSON_GetObjectItemCaseSensitive(bed, "temperature");
        if (cJSON_IsNumber(t)) s_status.bed_temp = (float)t->valuedouble;
        cJSON *g = cJSON_GetObjectItemCaseSensitive(bed, "target");
        if (cJSON_IsNumber(g)) s_status.bed_target = (float)g->valuedouble;
    }

    cJSON *ex = cJSON_GetObjectItemCaseSensitive(status, "extruder");
    if (cJSON_IsObject(ex)) {
        cJSON *t = cJSON_GetObjectItemCaseSensitive(ex, "temperature");
        if (cJSON_IsNumber(t)) s_status.extruder_temp = (float)t->valuedouble;
    }

    // Active tool: toolhead.extruder is "extruder" (T0), "extruder1" (T1), ... On
    // a multi-tool printer this picks which slot's filament is actually printing.
    cJSON *th = cJSON_GetObjectItemCaseSensitive(status, "toolhead");
    if (cJSON_IsObject(th)) {
        cJSON *ae = cJSON_GetObjectItemCaseSensitive(th, "extruder");
        if (cJSON_IsString(ae) && strncmp(ae->valuestring, "extruder", 8) == 0) {
            int idx = 0;
            for (const char *d = ae->valuestring + 8; *d >= '0' && *d <= '9'; ++d) idx = idx * 10 + (*d - '0');
            s_active_tool = idx;
            recompute_material();
        }
    }

    // Chamber: subscribed as "heater_generic chamber". Not every install has
    // it — cJSON just returns NULL and we leave chamber_temp as NaN.
    cJSON *chamber = cJSON_GetObjectItemCaseSensitive(status, "heater_generic chamber");
    if (cJSON_IsObject(chamber)) {
        cJSON *t = cJSON_GetObjectItemCaseSensitive(chamber, "temperature");
        if (cJSON_IsNumber(t)) s_status.chamber_temp = (float)t->valuedouble;
    }

    // Material: read from save_variables.variables.material. Users opt in by
    // adding `SAVE_VARIABLE VARIABLE=material VALUE='"{material}"'` to their
    // PRINT_START macro. Uppercased so PLA/pla/Pla all compare equal.
    cJSON *sv = cJSON_GetObjectItemCaseSensitive(status, "save_variables");
    if (cJSON_IsObject(sv)) {
        cJSON *vars = cJSON_GetObjectItemCaseSensitive(sv, "variables");
        if (cJSON_IsObject(vars)) {
            cJSON *m = cJSON_GetObjectItemCaseSensitive(vars, "material");
            if (cJSON_IsString(m)) {
                copy_upper(s_mat_sv, sizeof(s_mat_sv), m->valuestring);
                recompute_material();
            }
        }
    }

    if (state_dirty) recompute_printer_state();
}

static void send_subscribe(void);

// A complete Moonraker JSON-RPC frame has arrived. Route it.
static void handle_frame(const char *json, size_t len)
{
    cJSON *root = cJSON_ParseWithLength(json, len);
    if (root == NULL) {
        ESP_LOGW(TAG, "JSON parse failed");
        return;
    }

    // Subscribe response: {result:{status:{...}}}
    cJSON *result = cJSON_GetObjectItemCaseSensitive(root, "result");
    if (cJSON_IsObject(result)) {
        cJSON *status = cJSON_GetObjectItemCaseSensitive(result, "status");
        if (cJSON_IsObject(status)) {
            xSemaphoreTake(s_lock, portMAX_DELAY);
            merge_status_object(status);
            s_status.state = DC_MK_SUBSCRIBED;
            xSemaphoreGive(s_lock);
            ESP_LOGI(TAG, "subscribed; initial state=%s bed=%.1f material=%s",
                     dc_printer_state_str(s_status.printer),
                     s_status.bed_temp,
                     s_status.material[0] ? s_status.material : "?");
            cJSON_Delete(root);
            maybe_fetch_metadata();   // a print may already be loaded
            return;
        }
        // server.files.metadata reply: {"result":{"filament_type":"PLA;ABS;..."}}.
        // Split the comma/semicolon-separated per-tool list; recompute_material
        // then picks the active tool's entry. save_variables (if any) still wins.
        cJSON *ft = cJSON_GetObjectItemCaseSensitive(result, "filament_type");
        if (cJSON_IsString(ft) && ft->valuestring[0]) {
            xSemaphoreTake(s_lock, portMAX_DELAY);
            s_fil_count = 0;
            const char *p = ft->valuestring;
            const int max_tools = (int)(sizeof(s_fil_list) / sizeof(s_fil_list[0]));
            while (*p && s_fil_count < max_tools) {
                char *dst = s_fil_list[s_fil_count];
                size_t k = 0;
                while (*p && *p != ',' && *p != ';' && k < sizeof(s_fil_list[0]) - 1) {
                    dst[k++] = (char)toupper((unsigned char)*p);
                    ++p;
                }
                while (k > 0 && dst[k - 1] == ' ') --k;   // trim trailing space
                dst[k] = '\0';
                if (dst[0]) ++s_fil_count;
                if (*p == ',' || *p == ';') ++p;
            }
            recompute_material();
            char mat[16];
            strncpy(mat, s_status.material, sizeof(mat) - 1);
            mat[sizeof(mat) - 1] = '\0';
            int cnt = s_fil_count, tool = s_active_tool;
            xSemaphoreGive(s_lock);
            ESP_LOGI(TAG, "metadata: %d filament(s), active tool %d -> %s", cnt, tool, mat);
        }
        cJSON_Delete(root);
        return;
    }

    cJSON *method = cJSON_GetObjectItemCaseSensitive(root, "method");
    if (cJSON_IsString(method)) {
        const char *m = method->valuestring;

        // notify_status_update: {method:"notify_status_update", params:[{...}, eventtime]}
        if (strcmp(m, "notify_status_update") == 0) {
            cJSON *params = cJSON_GetObjectItemCaseSensitive(root, "params");
            if (cJSON_IsArray(params)) {
                cJSON *delta = cJSON_GetArrayItem(params, 0);
                xSemaphoreTake(s_lock, portMAX_DELAY);
                merge_status_object(delta);
                xSemaphoreGive(s_lock);
            }
            maybe_fetch_metadata();   // fetch filament_type when a new print loads
        }
        // Klippy has just come back after a restart / firmware-restart.
        // Moonraker preserves subscriptions server-side but does NOT replay
        // notify_status_update for the state that changed while Klippy was
        // gone — clients are expected to re-subscribe here to get a fresh
        // snapshot. Without this, we sit on stale cached data forever after
        // any Klippy restart.
        else if (strcmp(m, "notify_klippy_ready") == 0) {
            ESP_LOGI(TAG, "notify_klippy_ready — re-subscribing");
            dc_evlog_add("klippy ready; resubscribing");
            send_subscribe();
        }
        // Klippy shutdown / disconnected: our cached print_stats + temps are
        // stale. Force webhooks.state so the derived printer state flips to
        // ERROR immediately and dc_policy holds the vent instead of acting
        // on values that predate the shutdown.
        else if (strcmp(m, "notify_klippy_shutdown") == 0 ||
                 strcmp(m, "notify_klippy_disconnected") == 0) {
            xSemaphoreTake(s_lock, portMAX_DELAY);
            strncpy(s_wh_state, "shutdown", sizeof(s_wh_state) - 1);
            s_wh_state[sizeof(s_wh_state) - 1] = '\0';
            recompute_printer_state();
            xSemaphoreGive(s_lock);
        }
    }

    cJSON_Delete(root);
}

// ---------- ws events ----------

static void send_subscribe(void)
{
    // Objects mirror what stock reads off Bambu MQTT — enough to derive the
    // six-state printer model plus bed / extruder / chamber temps, print
    // progress, and material (via save_variables). Klipper silently omits
    // objects that don't exist on this printer, so subscribing to
    // "heater_generic chamber" / "save_variables" is safe if the user
    // doesn't have them.
    const char *req =
        "{\"jsonrpc\":\"2.0\",\"method\":\"printer.objects.subscribe\","
        "\"params\":{\"objects\":{"
            "\"webhooks\":null,"
            "\"print_stats\":null,"
            "\"virtual_sdcard\":null,"
            "\"heater_bed\":null,"
            "\"extruder\":null,"
            "\"toolhead\":null,"
            "\"heater_generic chamber\":null,"
            "\"save_variables\":null"
        "}},"
        "\"id\":1}";
    int sent = esp_websocket_client_send_text(s_ws, req, strlen(req),
                                              pdMS_TO_TICKS(NETWORK_TIMEOUT_MS));
    if (sent < 0) ESP_LOGW(TAG, "subscribe send failed");
    s_meta_file[0] = '\0';   // re-fetch metadata after a (re)subscribe
}

// Ask Moonraker for a file's metadata; the reply carries the slicer's
// filament_type, so we can auto-detect material with no save_variables macro.
static void send_metadata_request(const char *filename)
{
    if (s_ws == NULL || filename == NULL || filename[0] == '\0') return;
    char esc[128];
    size_t k = 0;
    for (const char *p = filename; *p && k < sizeof(esc) - 2; ++p) {
        if (*p == '"' || *p == '\\') esc[k++] = '\\';   // JSON-escape the name
        esc[k++] = *p;
    }
    esc[k] = '\0';
    char req[256];
    int n = snprintf(req, sizeof(req),
        "{\"jsonrpc\":\"2.0\",\"method\":\"server.files.metadata\","
        "\"params\":{\"filename\":\"%s\"},\"id\":2}", esc);
    if (n > 0 && n < (int)sizeof(req)) {
        esp_websocket_client_send_text(s_ws, req, strlen(req), pdMS_TO_TICKS(NETWORK_TIMEOUT_MS));
    }
}

// After a status merge, request metadata once per new print filename.
static void maybe_fetch_metadata(void)
{
    char want[64];
    xSemaphoreTake(s_lock, portMAX_DELAY);
    strncpy(want, s_status.filename, sizeof(want) - 1);
    want[sizeof(want) - 1] = '\0';
    bool need = want[0] && strcmp(want, s_meta_file) != 0;
    if (need) {
        strncpy(s_meta_file, want, sizeof(s_meta_file) - 1);
        s_meta_file[sizeof(s_meta_file) - 1] = '\0';
    }
    xSemaphoreGive(s_lock);
    if (need) send_metadata_request(want);
}

// Frames can arrive split across multiple DATA events. We buffer partials and
// dispatch once payload_offset+data_len == payload_len.
static void on_data(esp_websocket_event_data_t *ev)
{
    if (ev->op_code != 0x01 && ev->op_code != 0x00) return;   // text / continuation
    if (ev->payload_len <= 0) return;

    if (ev->payload_offset == 0) s_rx_off = 0;
    if (s_rx_off + ev->data_len >= RX_BUF_BYTES) {
        ESP_LOGW(TAG, "rx buffer overflow (payload_len=%d); dropping", ev->payload_len);
        s_rx_off = 0;
        return;
    }
    memcpy(s_rx_buf + s_rx_off, ev->data_ptr, ev->data_len);
    s_rx_off += ev->data_len;

    if (ev->payload_offset + ev->data_len >= ev->payload_len) {
        handle_frame(s_rx_buf, s_rx_off);
        s_rx_off = 0;
    }
}

static void ws_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    esp_websocket_event_data_t *ev = data;
    switch ((esp_websocket_event_id_t)id) {
    case WEBSOCKET_EVENT_CONNECTED:
        ESP_LOGI(TAG, "connected");
        dc_evlog_add("moonraker connected");
        xSemaphoreTake(s_lock, portMAX_DELAY);
        s_status.state = DC_MK_CONNECTED;
        xSemaphoreGive(s_lock);
        send_subscribe();
        break;
    case WEBSOCKET_EVENT_DATA:
        on_data(ev);
        break;
    case WEBSOCKET_EVENT_DISCONNECTED:
    case WEBSOCKET_EVENT_ERROR:
        xSemaphoreTake(s_lock, portMAX_DELAY);
        if (s_status.state != DC_MK_DISCONNECTED) {
            // Avoid a log-flood: only note the first transition to
            // disconnected. Repeat reconnect attempts stay quiet.
            dc_evlog_add("moonraker disconnected");
        }
        s_status.state = DC_MK_DISCONNECTED;
        // A dropped websocket says nothing about the printer itself, but we
        // also can't trust our cached state to still be current. Fall back
        // to UNKNOWN so dc_policy holds current target instead of acting on
        // stale data.
        s_status.printer = DC_PRINTER_UNKNOWN;
        s_status.printing = false;
        xSemaphoreGive(s_lock);
        break;
    default: break;
    }
}

// ---------- lifecycle ----------

static void stop_client(void)
{
    if (s_ws) {
        esp_websocket_client_close(s_ws, pdMS_TO_TICKS(1000));
        esp_websocket_client_destroy(s_ws);
        s_ws = NULL;
    }
}

static esp_err_t start_client(void)
{
    if (s_cfg.host[0] == '\0') {
        s_status.state = DC_MK_DISABLED;
        return ESP_OK;
    }
    char uri[128];
    // The uint16_t port and the int DEFAULT_PORT promote to int in the ?: result;
    // cast to unsigned to match the %u conversion (value is always a positive port,
    // so the printed text is identical — this only fixes the type mismatch).
    snprintf(uri, sizeof(uri), "ws://%s:%u/websocket",
             s_cfg.host, (unsigned)(s_cfg.port ? s_cfg.port : DEFAULT_PORT));

    esp_websocket_client_config_t wc = {
        .uri                  = uri,
        .reconnect_timeout_ms = RECONNECT_TIMEOUT_MS,
        .network_timeout_ms   = NETWORK_TIMEOUT_MS,
    };
    s_ws = esp_websocket_client_init(&wc);
    if (s_ws == NULL) return ESP_FAIL;

    esp_err_t err = esp_websocket_register_events(s_ws, WEBSOCKET_EVENT_ANY, ws_event, NULL);
    if (err == ESP_OK) err = esp_websocket_client_start(s_ws);
    if (err != ESP_OK) {
        stop_client();
        return err;
    }
    s_status.state = DC_MK_CONNECTING;
    ESP_LOGI(TAG, "connecting to %s", uri);
    return ESP_OK;
}

esp_err_t dc_moonraker_start(void)
{
    if (s_lock != NULL) return ESP_ERR_INVALID_STATE;
    s_lock = xSemaphoreCreateMutex();
    if (s_lock == NULL) return ESP_ERR_NO_MEM;

    s_rx_buf = malloc(RX_BUF_BYTES);
    if (s_rx_buf == NULL) return ESP_ERR_NO_MEM;

    esp_err_t err = nvs_load(&s_cfg);
    if (err != ESP_OK || s_cfg.host[0] == '\0') {
        ESP_LOGI(TAG, "no Moonraker config saved; idle");
        s_status.state = DC_MK_DISABLED;
        return ESP_OK;
    }
    return start_client();
}

esp_err_t dc_moonraker_set_config(const dc_moonraker_config_t *cfg)
{
    if (cfg == NULL) return ESP_ERR_INVALID_ARG;
    esp_err_t err = nvs_save(cfg);
    if (err != ESP_OK) return err;

    if (!s_lock) {
        s_cfg = *cfg;
        return ESP_OK;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_cfg = *cfg;
    stop_client();
    err = start_client();
    xSemaphoreGive(s_lock);
    return err;
}

esp_err_t dc_moonraker_get_config(dc_moonraker_config_t *out)
{
    if (out == NULL) return ESP_ERR_INVALID_ARG;
    if (!s_lock) {
        dc_moonraker_config_t persisted;
        if (nvs_load(&persisted) == ESP_OK) {
            *out = persisted;
        } else {
            *out = s_cfg;
        }
        return ESP_OK;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    *out = s_cfg;
    xSemaphoreGive(s_lock);
    return ESP_OK;
}

esp_err_t dc_moonraker_get_status(dc_moonraker_status_t *out)
{
    if (out == NULL) return ESP_ERR_INVALID_ARG;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    *out = s_status;
    xSemaphoreGive(s_lock);
    return ESP_OK;
}

esp_err_t dc_moonraker_clear_config(void)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;
    nvs_erase_key(h, KEY_HOST);
    nvs_erase_key(h, KEY_PORT);
    nvs_erase_key(h, KEY_APIKEY);
    nvs_commit(h);
    nvs_close(h);
    if (s_lock) {
        xSemaphoreTake(s_lock, portMAX_DELAY);
        memset(&s_cfg, 0, sizeof(s_cfg));
        xSemaphoreGive(s_lock);
    } else {
        memset(&s_cfg, 0, sizeof(s_cfg));
    }
    return ESP_OK;
}
