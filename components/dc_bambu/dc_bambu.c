// Bambu Lab LAN MQTT client. Reads the printer's live bed temperature over its
// on-device MQTT-over-TLS broker so AUTO can follow a Bambu print, mirroring the
// Moonraker path. Read-only — no control commands are ever sent to the printer.
//
// Validated against a real Bambu P1S (2026-08-11): connects, subscribes, and
// decodes live report data. Built from the OpenBambuAPI / ha-bambulab protocol
// spec; other models remain unvalidated. Protocol:
//   mqtts://<host>:8883, user "bblp", pass = LAN access code, self-signed cert
//   (CN=serial, connect by IP -> cert verification relaxed). Subscribe
//   device/<serial>/report; publish one "pushall" on connect (P1/A1 send deltas).
// See plans/control-source-bambu-ha.md.
#include "dc_bambu.h"
#include "dc_bambu_parse.h"

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "mqtt_client.h"
#include "nvs.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>   // strncasecmp / strcasecmp for filament zone matching

// Bambu's broker presents a per-device SELF-SIGNED cert (CN=serial) and we reach
// it by IP, so there is no CA to verify against and the client below deliberately
// connects without server-cert verification. esp-tls only permits that when these
// are enabled; without them it refuses to build the TLS context at all and every
// connect fails with "No server verification option set in esp_tls_cfg_t" —
// which reads like a network or credential fault, not a missing build option.
//
// Asserted here rather than left to a comment because a product supplying neither
// still compiles, links and runs, and only fails on a real connection attempt.
// DragonVent shipped in exactly that state; see justinh-rahb/DragonVent#13.
#if !defined(CONFIG_ESP_TLS_SKIP_SERVER_CERT_VERIFY) || !defined(CONFIG_ESP_TLS_INSECURE)
#error "dc_bambu requires CONFIG_ESP_TLS_INSECURE=y and CONFIG_ESP_TLS_SKIP_SERVER_CERT_VERIFY=y \
in the product's sdkconfig.defaults. Bambu's LAN broker uses a per-device self-signed cert reached \
by IP, so there is no CA to verify against; without these esp-tls fails SSL setup and the client can \
never connect. Add both, or drop the dc_bambu dependency."
#endif

static const char *TAG = "dc_bambu";

#define NVS_NS   "app_nvs"
#define KEY_HOST "bb_host"
#define KEY_SER  "bb_serial"
#define KEY_CODE "bb_code"

// A full "pushall" report is ~10-15 KB. esp-mqtt fragments payloads larger than
// its RX buffer; we reassemble up to RX_CAP (bed_temper is near the front of the
// print object, so a truncated tail still yields the follow signal).
#define MQTT_BUF   8192
#define RX_CAP     16384
#define COMPLETION_HOLD_US 7500000LL

// One pushall on connect; required on P1/A1 (delta-only), harmless on X1.
static const char PUSHALL[] =
    "{\"pushing\":{\"sequence_id\":\"0\",\"command\":\"pushall\",\"version\":1,\"push_target\":1}}";

static SemaphoreHandle_t        s_lock  = NULL;
static dc_bambu_config_t        s_cfg   = {0};
static dc_bambu_status_t        s_status = {
    .state = DC_BAMBU_DISABLED, .bed_temp = NAN, .bed_target = NAN, .chamber_temp = NAN,
    .chamber_temp_age_ms = UINT32_MAX, .progress = -1,
};
static esp_mqtt_client_handle_t s_client = NULL;

static char   s_report_topic[80]  = {0};   // device/<serial>/report
static char   s_request_topic[80] = {0};   // device/<serial>/request
static char  *s_rx = NULL;                 // RX_CAP reassembly buffer
static size_t s_rx_len = 0;
static bool   s_in_report = false;         // current inbound msg is on the report topic
static int64_t s_chamber_temp_us = 0;      // monotonic timestamp of last chamber_temper sample
static dc_bambu_gcode_phase_t s_gcode_phase = DC_BAMBU_GCODE_UNKNOWN;
static uint32_t s_print_error_code = 0;
static bool s_has_observed_active_job = false;
static bool s_completion_visible = false;
static int64_t s_completion_until_us = 0;

// ---------- NVS ----------

static esp_err_t nvs_load(dc_bambu_config_t *out)
{
    memset(out, 0, sizeof(*out));
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NS, NVS_READONLY, &h);
    if (err != ESP_OK) return err;

    size_t sz = sizeof(out->host);
    err = nvs_get_str(h, KEY_HOST, out->host, &sz);
    if (err != ESP_OK) { nvs_close(h); return err; }   // no host = unconfigured

    sz = sizeof(out->serial);
    nvs_get_str(h, KEY_SER, out->serial, &sz);
    sz = sizeof(out->code);
    nvs_get_str(h, KEY_CODE, out->code, &sz);
    nvs_close(h);
    return ESP_OK;
}

static esp_err_t nvs_save(const dc_bambu_config_t *cfg)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;
    err = nvs_set_str(h, KEY_HOST, cfg->host);
    if (err == ESP_OK) err = nvs_set_str(h, KEY_SER, cfg->serial);
    if (err == ESP_OK) err = nvs_set_str(h, KEY_CODE, cfg->code);
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    return err;
}

// ---------- report parsing ----------

// Pull the float value of a JSON key via a targeted scan (no full JSON parse — the
// C3 can't afford a parse tree over a 15 KB payload). `key` includes the quotes,
// e.g. "\"bed_temper\"".
static bool find_float(const char *s, const char *key, float *out)
{
    const char *q = strstr(s, key);
    if (!q) return false;
    q = strchr(q, ':');
    if (!q) return false;
    char *end;
    float v = strtof(q + 1, &end);
    if (end == q + 1) return false;   // no number parsed
    *out = v;
    return true;
}

// find_string() + active_filament() (tri-state) live in dc_bambu_parse.h so they
// can be host-unit-tested (tests/dc_bambu_host_test.c).

static void parse_report(const char *json)
{
    float bed, bedtgt, cham, percent;
    bool got_bed  = find_float(json, "\"bed_temper\"", &bed);
    // bed_target_temper is the commanded setpoint AUTO triggers on. Match the
    // "_target_temper" so it can't be confused with "bed_temper" (strstr finds the
    // first hit; searching the more specific key avoids the prefix collision).
    bool got_tgt  = find_float(json, "bed_target_temper", &bedtgt);
    bool got_cham = find_float(json, "\"chamber_temper\"", &cham);
    bool got_percent = find_float(json, "\"mc_percent\"", &percent);
    // NOTE(phase 2b): H2/newer moved chamber temp to a packed device.ctc.info.temp
    // field; only the legacy flat chamber_temper is read here. Bed follow (the
    // goal) works on all models via bed_temper/bed_target_temper.
    char fila[16];
    dc_fila_result_t fr = dc_bambu_active_filament(json, fila, sizeof fila);
    char gs[16];
    bool got_gs = dc_bambu_find_string(json, "\"gcode_state\"", gs, sizeof gs);  // print state
    uint32_t print_error_code = 0;
    bool got_print_error = dc_bambu_print_error_code(json, &print_error_code);

    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (got_bed) {
        s_status.bed_temp  = bed;
        s_status.state     = DC_BAMBU_SUBSCRIBED;   // we have live data now
        s_status.connected = true;
    }
    if (got_tgt)  s_status.bed_target = bedtgt;
    if (got_cham) {
        s_status.chamber_temp = cham;
        s_chamber_temp_us = esp_timer_get_time();
        s_status.chamber_temp_age_ms = 0;
    }
    if (got_percent) s_status.progress = percent < 0 ? 0 : percent > 100 ? 1 : percent / 100.0f;
    if (got_gs) s_gcode_phase = dc_bambu_gcode_phase(gs);
    if (got_print_error) s_print_error_code = print_error_code;
    if (got_gs || got_print_error) {
        s_status.printing = s_gcode_phase == DC_BAMBU_GCODE_PREPARING ||
                            s_gcode_phase == DC_BAMBU_GCODE_PRINTING ||
                            s_gcode_phase == DC_BAMBU_GCODE_PAUSED;
        s_status.error = s_print_error_code != 0;
        if (s_status.error) {
            s_has_observed_active_job = false;
            s_completion_visible = false;
            s_completion_until_us = 0;
            s_status.print_state = DC_BAMBU_PRINT_ERROR;
        } else switch (s_gcode_phase) {
        case DC_BAMBU_GCODE_IDLE:
            s_has_observed_active_job = false;
            s_completion_visible = false;
            s_completion_until_us = 0;
            s_status.print_state = DC_BAMBU_PRINT_IDLE;
            break;
        case DC_BAMBU_GCODE_DOWNLOADING: s_status.print_state = DC_BAMBU_PRINT_DOWNLOADING; break;
        case DC_BAMBU_GCODE_PREPARING:   s_status.print_state = DC_BAMBU_PRINT_PREPARING; break;
        case DC_BAMBU_GCODE_PRINTING:
            s_has_observed_active_job = true;
            s_completion_visible = false;
            s_completion_until_us = 0;
            s_status.print_state = DC_BAMBU_PRINT_PRINTING;
            break;
        case DC_BAMBU_GCODE_PAUSED:
            s_has_observed_active_job = true;
            s_completion_visible = false;
            s_completion_until_us = 0;
            s_status.print_state = DC_BAMBU_PRINT_PAUSED;
            break;
        case DC_BAMBU_GCODE_COMPLETE: {
            int64_t now_us = esp_timer_get_time();
            if (s_has_observed_active_job && !s_completion_visible) {
                s_has_observed_active_job = false;
                s_completion_visible = true;
                s_completion_until_us = now_us + COMPLETION_HOLD_US;
            }
            if (s_completion_visible && now_us < s_completion_until_us) {
                s_status.print_state = DC_BAMBU_PRINT_COMPLETE;
            } else {
                s_completion_visible = false;
                s_completion_until_us = 0;
                s_status.print_state = DC_BAMBU_PRINT_IDLE;
            }
            break;
        }
        case DC_BAMBU_GCODE_ERROR:
            // Bambu uses FAILED with print_error=0 for a deliberate stop.
            s_has_observed_active_job = false;
            s_completion_visible = false;
            s_completion_until_us = 0;
            s_status.print_state = DC_BAMBU_PRINT_IDLE;
            break;
        default:                          s_status.print_state = DC_BAMBU_PRINT_UNKNOWN; break;
        }
    }
    // Tri-state: PRESENT updates the filament; EMPTY (unload / print end / no spool)
    // CLEARS it so a stale zone is never applied; ABSENT (a delta that simply omits
    // the AMS/tray block) leaves the last known value untouched.
    bool fila_changed = false;
    if (fr == DC_FILA_PRESENT && strcmp(fila, s_status.filament) != 0) {
        snprintf(s_status.filament, sizeof s_status.filament, "%s", fila);
        fila_changed = true;
    } else if (fr == DC_FILA_EMPTY && s_status.filament[0]) {
        s_status.filament[0] = '\0';
        fila_changed = true;
    }
    xSemaphoreGive(s_lock);

    if (fila_changed) ESP_LOGI(TAG, "active filament: %s", fila[0] ? fila : "(none)");
    if (got_bed) ESP_LOGD(TAG, "bed=%.1f chamber=%.1f", bed, got_cham ? cham : NAN);
}

static bool topic_is_report(const char *topic, int len)
{
    return len > 0 && (size_t)len == strlen(s_report_topic)
        && strncmp(topic, s_report_topic, (size_t)len) == 0;
}

// ---------- mqtt events ----------

static void mqtt_event_handler(void *args, esp_event_base_t base, int32_t id, void *data)
{
    (void)args; (void)base;
    esp_mqtt_event_handle_t e = (esp_mqtt_event_handle_t)data;
    switch ((esp_mqtt_event_id_t)id) {
    case MQTT_EVENT_CONNECTED:
        ESP_LOGI(TAG, "connected; subscribing to Bambu report topic");
        xSemaphoreTake(s_lock, portMAX_DELAY);
        s_status.state = DC_BAMBU_CONNECTED;   // not SUBSCRIBED until first report
        s_status.chamber_temp = NAN;
        s_status.chamber_temp_age_ms = UINT32_MAX;
        s_chamber_temp_us = 0;
        xSemaphoreGive(s_lock);
        esp_mqtt_client_subscribe(s_client, s_report_topic, 0);
        esp_mqtt_client_publish(s_client, s_request_topic, PUSHALL, 0, 0, 0);
        break;

    case MQTT_EVENT_DISCONNECTED:
        xSemaphoreTake(s_lock, portMAX_DELAY);
        s_status.state     = DC_BAMBU_DISCONNECTED;
        s_status.connected = false;
        s_status.chamber_temp = NAN;
        s_status.chamber_temp_age_ms = UINT32_MAX;
        s_chamber_temp_us = 0;
        xSemaphoreGive(s_lock);
        break;

    case MQTT_EVENT_DATA: {
        // Reassemble a possibly-fragmented payload. The topic is present only on
        // the first fragment (offset 0); track whether this message is the report.
        if (e->current_data_offset == 0) {
            s_in_report = topic_is_report(e->topic, e->topic_len);
            s_rx_len = 0;
        }
        if (!s_in_report || s_rx == NULL) break;
        size_t off = (size_t)e->current_data_offset;
        if (off < RX_CAP - 1) {
            size_t copy = (size_t)e->data_len;
            if (off + copy > RX_CAP - 1) copy = (RX_CAP - 1) - off;
            memcpy(s_rx + off, e->data, copy);
            s_rx_len = off + copy;
        }
        if (e->current_data_offset + e->data_len >= e->total_data_len) {
            s_rx[s_rx_len] = '\0';
            parse_report(s_rx);
        }
        break;
    }

    default:
        break;
    }
}

// ---------- lifecycle ----------

esp_err_t dc_bambu_start(void)
{
    if (s_lock != NULL) return ESP_ERR_INVALID_STATE;
    s_lock = xSemaphoreCreateMutex();
    if (s_lock == NULL) return ESP_ERR_NO_MEM;

    if (nvs_load(&s_cfg) != ESP_OK || s_cfg.host[0] == '\0') {
        ESP_LOGI(TAG, "no Bambu config saved; idle");
        s_status.state = DC_BAMBU_DISABLED;
        return ESP_OK;
    }
    if (s_cfg.serial[0] == '\0' || s_cfg.code[0] == '\0') {
        ESP_LOGW(TAG, "Bambu needs host + serial + access code; idle");
        s_status.state = DC_BAMBU_DISABLED;
        return ESP_OK;
    }

    s_rx = malloc(RX_CAP);
    if (s_rx == NULL) return ESP_ERR_NO_MEM;
    snprintf(s_report_topic,  sizeof s_report_topic,  "device/%s/report",  s_cfg.serial);
    snprintf(s_request_topic, sizeof s_request_topic, "device/%s/request", s_cfg.serial);

    char uri[96];
    snprintf(uri, sizeof uri, "mqtts://%s:8883", s_cfg.host);
    esp_mqtt_client_config_t mc = {
        .broker.address.uri = uri,
        // Self-signed per-device cert (CN=serial) reached by IP: no CA to verify
        // against, so a read-only LAN client connects WITHOUT server-cert
        // verification. With no CA/bundle set, esp-tls falls through to VERIFY_NONE
        // only because CONFIG_ESP_TLS_SKIP_SERVER_CERT_VERIFY is enabled (see
        // sdkconfig.defaults) — otherwise it errors "No server verification option
        // set" and the connect fails. skip the CN check too since IP != serial.
        .broker.verification.skip_cert_common_name_check = true,
        .broker.verification.use_global_ca_store = false,
        .credentials.username = "bblp",
        .credentials.authentication.password = s_cfg.code,
        .buffer.size = MQTT_BUF,
    };

    s_client = esp_mqtt_client_init(&mc);
    if (s_client == NULL) {
        ESP_LOGE(TAG, "esp_mqtt_client_init failed");
        free(s_rx); s_rx = NULL;
        s_status.state = DC_BAMBU_DISCONNECTED;
        return ESP_FAIL;
    }
    esp_mqtt_client_register_event(s_client, ESP_EVENT_ANY_ID, mqtt_event_handler, NULL);
    esp_err_t err = esp_mqtt_client_start(s_client);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_mqtt_client_start: %s", esp_err_to_name(err));
        s_status.state = DC_BAMBU_DISCONNECTED;
        return err;
    }
    s_status.state = DC_BAMBU_CONNECTING;
    ESP_LOGI(TAG, "connecting to %s", uri);
    return ESP_OK;
}

esp_err_t dc_bambu_set_config(const dc_bambu_config_t *cfg)
{
    if (cfg == NULL) return ESP_ERR_INVALID_ARG;
    esp_err_t err = nvs_save(cfg);
    if (err != ESP_OK) return err;
    if (s_lock) {
        xSemaphoreTake(s_lock, portMAX_DELAY);
        s_cfg = *cfg;
        xSemaphoreGive(s_lock);
    } else {
        s_cfg = *cfg;
    }
    return ESP_OK;   // takes effect on next boot (matches dc_moonraker semantics)
}

esp_err_t dc_bambu_get_config(dc_bambu_config_t *out)
{
    if (out == NULL) return ESP_ERR_INVALID_ARG;
    if (!s_lock) {
        dc_bambu_config_t persisted;
        if (nvs_load(&persisted) == ESP_OK) {
            *out = persisted;
        } else {
            *out = s_cfg;
        }
        return ESP_OK;
    }
    if (s_lock) xSemaphoreTake(s_lock, portMAX_DELAY);
    *out = s_cfg;
    if (s_lock) xSemaphoreGive(s_lock);
    return ESP_OK;
}

esp_err_t dc_bambu_get_status(dc_bambu_status_t *out)
{
    if (out == NULL) return ESP_ERR_INVALID_ARG;
    if (s_lock) xSemaphoreTake(s_lock, portMAX_DELAY);
    *out = s_status;
    int64_t sample_us = s_chamber_temp_us;
    int64_t completion_until_us = s_completion_until_us;
    if (s_lock) xSemaphoreGive(s_lock);

    int64_t now_us = esp_timer_get_time();
    // X1 printers can leave gcode_state at FINISH indefinitely. Show completion
    // briefly, then resolve that stale report to idle without another message.
    if (out->print_state == DC_BAMBU_PRINT_COMPLETE &&
        (completion_until_us <= 0 || now_us >= completion_until_us)) {
        out->print_state = DC_BAMBU_PRINT_IDLE;
        out->printing = false;
    }
    if (sample_us <= 0 || !isfinite(out->chamber_temp)) {
        out->chamber_temp = NAN;
        out->chamber_temp_age_ms = UINT32_MAX;
    } else {
        int64_t age_us = now_us - sample_us;
        if (age_us < 0) age_us = 0;
        uint64_t age_ms = (uint64_t)age_us / 1000ULL;
        out->chamber_temp_age_ms =
            age_ms > UINT32_MAX ? UINT32_MAX : (uint32_t)age_ms;
        if (!dc_bambu_chamber_sample_fresh(now_us, sample_us))
            out->chamber_temp = NAN;
    }
    return ESP_OK;
}

esp_err_t dc_bambu_clear_config(void)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;
    nvs_erase_key(h, KEY_HOST);
    nvs_erase_key(h, KEY_SER);
    nvs_erase_key(h, KEY_CODE);
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

// ---------- filament chamber zones (issue #64, Bambu only) ----------

// Built-in filament types: display name + per-key NVS target override + a default
// (shown as the UI's "default N" hint). PLA/TPU off (a hot chamber hurts PLA);
// PETG/ABS-ASA/PA/PC want warmth for adhesion / reduced warping. ABS and ASA share
// one combined zone (identical chamber needs); to diverge, add a custom ABS or ASA
// profile — it overrides the combined default (see dc_bambu_zone_target).
static const char *const ZONE_NAMES[DC_BAMBU_ZONE_COUNT] = { "PLA", "PETG", "ABS/ASA", "PA", "PC", "TPU" };
static const char *const ZONE_KEYS [DC_BAMBU_ZONE_COUNT] = { "zone_pla", "zone_petg", "zone_abs", "zone_pa", "zone_pc", "zone_tpu" };
//                                                            PLA PETG ABS/ASA PA PC  TPU
static const uint8_t     ZONE_DEF  [DC_BAMBU_ZONE_COUNT] = {  0,  40,   55,    50, 60,  0 };

// Prefix(es) a Bambu filament report is matched against, per built-in. The combined
// zone matches BOTH "ABS…" and "ASA…"; the rest match their own name. NULL-terminated.
static const char *const ZONE_MATCH[DC_BAMBU_ZONE_COUNT][3] = {
    { "PLA",  NULL },
    { "PETG", NULL },
    { "ABS",  "ASA", NULL },   // combined ABS/ASA
    { "PA",   NULL },
    { "PC",   NULL },
    { "TPU",  NULL },
};

// User custom profiles — persisted together as one NVS blob "zone_custom".
typedef struct { char name[12]; uint8_t target_c; } custom_zone_t;

static uint8_t       s_zone_c[DC_BAMBU_ZONE_COUNT];   // built-in target cache
static custom_zone_t s_custom[DC_BAMBU_CUSTOM_MAX];   // custom profile cache
static int           s_custom_n = 0;
static bool          s_zones_loaded = false;

// Load built-in targets (NVS override, else default) + custom profiles into RAM.
static void zones_load(void)
{
    nvs_handle_t h;
    bool have = (nvs_open(NVS_NS, NVS_READONLY, &h) == ESP_OK);
    for (int i = 0; i < DC_BAMBU_ZONE_COUNT; i++) {
        uint8_t v = ZONE_DEF[i];
        if (have) nvs_get_u8(h, ZONE_KEYS[i], &v);   // leaves default if key absent
        s_zone_c[i] = v;
    }
    s_custom_n = 0;
    if (have) {
        size_t len = sizeof s_custom;
        if (nvs_get_blob(h, "zone_custom", s_custom, &len) == ESP_OK)
            s_custom_n = (int)(len / sizeof(custom_zone_t));
        nvs_close(h);
    }
    s_zones_loaded = true;
}

static esp_err_t customs_persist(void)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;
    if (s_custom_n > 0) err = nvs_set_blob(h, "zone_custom", s_custom, s_custom_n * sizeof(custom_zone_t));
    else                nvs_erase_key(h, "zone_custom");
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    return err;
}

uint8_t dc_bambu_zone_target(const char *filament)
{
    if (!s_zones_loaded) zones_load();
    if (!filament || !filament[0]) return 0;
    // Longest-prefix match over customs + built-in aliases (a custom "PETG-CF" beats
    // "PETG"). Customs are listed FIRST so that on an EQUAL-length tie the custom
    // wins — this is what lets a custom "ABS"/"ASA" override the combined built-in
    // (the matcher keeps the first of equal length). Built-ins expand to their match
    // prefixes, so the combined zone contributes both "ABS" and "ASA".
    const char *names[DC_BAMBU_CUSTOM_MAX + DC_BAMBU_ZONE_COUNT + 2];
    uint8_t     temps[DC_BAMBU_CUSTOM_MAX + DC_BAMBU_ZONE_COUNT + 2];
    int n = 0;
    for (int i = 0; i < s_custom_n; i++) { names[n] = s_custom[i].name; temps[n] = s_custom[i].target_c; n++; }
    for (int i = 0; i < DC_BAMBU_ZONE_COUNT; i++)
        for (int a = 0; ZONE_MATCH[i][a]; a++) { names[n] = ZONE_MATCH[i][a]; temps[n] = s_zone_c[i]; n++; }
    int idx = dc_bambu_zone_match(filament, names, n);
    return idx < 0 ? 0 : temps[idx];
}

int dc_bambu_zone_get_all(dc_bambu_zone_t *out, int max)
{
    if (!s_zones_loaded) zones_load();
    int n = 0;
    for (int i = 0; i < DC_BAMBU_ZONE_COUNT && n < max; i++, n++) {
        snprintf(out[n].name, sizeof out[n].name, "%s", ZONE_NAMES[i]);
        out[n].target_c  = s_zone_c[i];
        out[n].default_c = ZONE_DEF[i];
        out[n].custom    = false;
    }
    for (int i = 0; i < s_custom_n && n < max; i++, n++) {
        snprintf(out[n].name, sizeof out[n].name, "%.11s", s_custom[i].name);
        out[n].target_c  = s_custom[i].target_c;
        out[n].default_c = 0;
        out[n].custom    = true;
    }
    return n;
}

esp_err_t dc_bambu_zone_set(const char *name, uint8_t target_c)
{
    if (!name || !name[0]) return ESP_ERR_INVALID_ARG;
    if (target_c > 70) target_c = 70;   // settable ceiling; dc_policy enforces hard cutoffs
    if (!s_zones_loaded) zones_load();

    // Built-in: update its NVS key + cache.
    for (int i = 0; i < DC_BAMBU_ZONE_COUNT; i++) {
        if (strcasecmp(name, ZONE_NAMES[i]) == 0) {
            nvs_handle_t h;
            esp_err_t err = nvs_open(NVS_NS, NVS_READWRITE, &h);
            if (err != ESP_OK) return err;
            err = nvs_set_u8(h, ZONE_KEYS[i], target_c);
            if (err == ESP_OK) err = nvs_commit(h);
            nvs_close(h);
            if (err == ESP_OK) s_zone_c[i] = target_c;
            return err;
        }
    }
    // Existing custom profile: update in place.
    for (int i = 0; i < s_custom_n; i++)
        if (strcasecmp(name, s_custom[i].name) == 0) {
            s_custom[i].target_c = target_c;
            return customs_persist();
        }
    // New custom profile: append if there's room and the name fits.
    if (s_custom_n >= DC_BAMBU_CUSTOM_MAX) return ESP_ERR_NO_MEM;
    if (strlen(name) >= sizeof s_custom[0].name) return ESP_ERR_INVALID_SIZE;
    memset(&s_custom[s_custom_n], 0, sizeof s_custom[0]);
    snprintf(s_custom[s_custom_n].name, sizeof s_custom[s_custom_n].name, "%.11s", name);
    s_custom[s_custom_n].target_c = target_c;
    s_custom_n++;
    return customs_persist();
}

esp_err_t dc_bambu_zone_remove(const char *name)
{
    if (!name || !name[0]) return ESP_ERR_INVALID_ARG;
    if (!s_zones_loaded) zones_load();
    for (int i = 0; i < s_custom_n; i++)
        if (strcasecmp(name, s_custom[i].name) == 0) {
            for (int j = i; j < s_custom_n - 1; j++) s_custom[j] = s_custom[j + 1];
            s_custom_n--;
            return customs_persist();
        }
    return ESP_ERR_NOT_FOUND;   // not a custom (built-ins can't be removed)
}
