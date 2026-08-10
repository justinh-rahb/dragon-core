// SPDX-License-Identifier: MIT
#include "dc_portal.h"
#include "dc_portal_dns.h"

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include "dc_evlog.h"
#include "dc_ui.h"
#include "dc_wifi.h"
#include "esp_app_desc.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/inet.h"
#include "mbedtls/sha256.h"

// Minimum stack for the portal's HTTP server task. See dc_portal_start().
#define DC_PORTAL_MIN_HTTPD_STACK 8192

static const char *TAG = "dc_portal";
static httpd_handle_t s_httpd;
static dc_portal_config_t s_config;
static bool s_ap_mode;

static esp_err_t send_json(httpd_req_t *req, cJSON *root)
{
    char *body = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!body) return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                                          "out of memory");
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    esp_err_t err = httpd_resp_sendstr(req, body);
    cJSON_free(body);
    return err;
}

static esp_err_t json_error(httpd_req_t *req, const char *status, const char *message)
{
    httpd_resp_set_status(req, status);
    cJSON *root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "ok", false);
    cJSON_AddStringToObject(root, "error", message);
    return send_json(req, root);
}

static cJSON *recv_json(httpd_req_t *req)
{
    if (req->content_len <= 0 || req->content_len > 4096) return NULL;
    char *body = malloc((size_t)req->content_len + 1);
    if (!body) return NULL;
    int offset = 0;
    while (offset < req->content_len) {
        int got = httpd_req_recv(req, body + offset, req->content_len - offset);
        if (got == HTTPD_SOCK_ERR_TIMEOUT) continue;
        if (got <= 0) { free(body); return NULL; }
        offset += got;
    }
    body[offset] = 0;
    cJSON *root = cJSON_Parse(body);
    free(body);
    return root;
}

static bool authorized(httpd_req_t *req)
{
    return !s_config.authorize || s_config.authorize(req, s_config.ctx);
}

static esp_err_t require_auth(httpd_req_t *req)
{
    if (authorized(req)) return ESP_OK;
    json_error(req, "403 Forbidden", "authorization required");
    return ESP_ERR_INVALID_STATE;
}

static esp_err_t spa_get(httpd_req_t *req)
{
    dc_ui_asset_t asset = dc_ui_spa_asset();
    httpd_resp_set_type(req, asset.content_type);
    httpd_resp_set_hdr(req, "Content-Encoding", asset.content_encoding);
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, (const char *)asset.data, asset.len);
}

static esp_err_t provisioning_get(httpd_req_t *req)
{
    // Captive provisioning must remain reachable on an unconfigured AP. Once
    // joined to a LAN, the product schema can contain operational settings and
    // must be protected by the product's normal authorization policy.
    if (!s_ap_mode && require_auth(req) != ESP_OK) return ESP_OK;
    cJSON *root = cJSON_CreateObject();
    cJSON_AddNumberToObject(root, "schema", 1);
    cJSON_AddStringToObject(root, "product", s_config.product ?: "dragon");
    cJSON_AddStringToObject(root, "display_name", s_config.display_name ?: "Dragon Device");
    cJSON_AddStringToObject(root, "network_mode", s_ap_mode ? "ap" : "station");
    cJSON_AddBoolToObject(root, "scanning", dc_wifi_is_scanning());

    char failed_ssid[33] = {0}, failed_reason[96] = {0};
    if (dc_wifi_last_sta_fail(failed_ssid, sizeof(failed_ssid),
                              failed_reason, sizeof(failed_reason))) {
        cJSON *failure = cJSON_AddObjectToObject(root, "last_failure");
        cJSON_AddStringToObject(failure, "ssid", failed_ssid);
        cJSON_AddStringToObject(failure, "reason", failed_reason);
    }

    wifi_ap_record_t records[DC_WIFI_SCAN_MAX];
    int count = dc_wifi_get_scan_results(records, DC_WIFI_SCAN_MAX);
    cJSON *networks = cJSON_AddArrayToObject(root, "networks");
    for (int i = 0; i < count; ++i) {
        cJSON *network = cJSON_CreateObject();
        cJSON_AddStringToObject(network, "ssid", (const char *)records[i].ssid);
        cJSON_AddNumberToObject(network, "rssi", records[i].rssi);
        cJSON_AddBoolToObject(network, "secured", records[i].authmode != WIFI_AUTH_OPEN);
        cJSON_AddItemToArray(networks, network);
    }

    dc_wifi_ap_config_t ap = {0};
    if (dc_wifi_get_ap_config(&ap) == ESP_OK) {
        cJSON *obj = cJSON_AddObjectToObject(root, "fallback_ap");
        cJSON_AddStringToObject(obj, "ssid", ap.ssid);
        cJSON_AddStringToObject(obj, "password", "");
        struct in_addr addr = { .s_addr = htonl(ap.ip) };
        char ip[INET_ADDRSTRLEN] = {0};
        inet_ntoa_r(addr, ip, sizeof(ip));
        cJSON_AddStringToObject(obj, "ip", ip);
        cJSON_AddBoolToObject(obj, "enabled", ap.enabled);
    }

    if (s_config.describe_product) {
        cJSON *product = s_config.describe_product(s_config.ctx);
        if (product) cJSON_AddItemToObject(root, "product_setup", product);
    }
    return send_json(req, root);
}

static esp_err_t scan_post(httpd_req_t *req)
{
    esp_err_t err = dc_wifi_scan_start();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE)
        return json_error(req, "500 Internal Server Error", esp_err_to_name(err));
    cJSON *root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "ok", true);
    cJSON_AddBoolToObject(root, "scanning", true);
    return send_json(req, root);
}

static esp_err_t wifi_post(httpd_req_t *req)
{
    if (!s_ap_mode && require_auth(req) != ESP_OK) return ESP_OK;
    cJSON *body = recv_json(req);
    cJSON *ssid = body ? cJSON_GetObjectItemCaseSensitive(body, "ssid") : NULL;
    cJSON *password = body ? cJSON_GetObjectItemCaseSensitive(body, "password") : NULL;
    if (!cJSON_IsString(ssid) || !ssid->valuestring[0] ||
        (password && !cJSON_IsString(password))) {
        cJSON_Delete(body);
        return json_error(req, "400 Bad Request", "ssid is required");
    }
    size_t ssid_len = strlen(ssid->valuestring);
    const char *password_text = password ? password->valuestring : "";
    size_t password_len = strlen(password_text);
    if (!dc_wifi_ssid_valid(ssid->valuestring, false)) {
        cJSON_Delete(body);
        return json_error(req, "400 Bad Request", "SSID must be 1-32 bytes");
    }
    if (!dc_wifi_password_valid(password_text)) {
        cJSON_Delete(body);
        return json_error(req, "400 Bad Request",
                          "Wi-Fi password must be blank, 8-63 characters, or a 64-digit hex key");
    }
    char saved_ssid[33] = {0}, saved_password[65] = {0};
    memcpy(saved_ssid, ssid->valuestring, ssid_len);
    memcpy(saved_password, password_text, password_len);
    cJSON_Delete(body);
    cJSON *reply = cJSON_CreateObject();
    cJSON_AddBoolToObject(reply, "ok", true);
    cJSON_AddBoolToObject(reply, "rebooting", true);
    send_json(req, reply);
    vTaskDelay(pdMS_TO_TICKS(250));
    return dc_wifi_save_creds_and_reboot(saved_ssid, saved_password);
}

static uint32_t parse_ip(const char *text)
{
    struct in_addr addr;
    return text && inet_aton(text, &addr) ? ntohl(addr.s_addr) : 0;
}

static esp_err_t ap_post(httpd_req_t *req)
{
    if (require_auth(req) != ESP_OK) return ESP_OK;
    cJSON *body = recv_json(req);
    if (!body) return json_error(req, "400 Bad Request", "invalid JSON");
    dc_wifi_ap_config_t config = {0};
    cJSON *ssid = cJSON_GetObjectItemCaseSensitive(body, "ssid");
    cJSON *password = cJSON_GetObjectItemCaseSensitive(body, "password");
    cJSON *ip = cJSON_GetObjectItemCaseSensitive(body, "ip");
    cJSON *enabled = cJSON_GetObjectItemCaseSensitive(body, "enabled");
    if ((ssid && !cJSON_IsString(ssid)) || (password && !cJSON_IsString(password))) {
        cJSON_Delete(body);
        return json_error(req, "400 Bad Request", "AP SSID and password must be strings");
    }
    size_t ssid_len = cJSON_IsString(ssid) ? strlen(ssid->valuestring) : 0;
    size_t password_len = cJSON_IsString(password) ? strlen(password->valuestring) : 0;
    if (!dc_wifi_ssid_valid(cJSON_IsString(ssid) ? ssid->valuestring : "", true)) {
        cJSON_Delete(body);
        return json_error(req, "400 Bad Request", "AP SSID must be at most 32 bytes");
    }
    if (!dc_wifi_password_valid(cJSON_IsString(password) ? password->valuestring : "")) {
        cJSON_Delete(body);
        return json_error(req, "400 Bad Request",
                          "AP password must be blank, 8-63 characters, or a 64-digit hex key");
    }
    if (ssid_len) memcpy(config.ssid, ssid->valuestring, ssid_len);
    if (password_len) memcpy(config.password, password->valuestring, password_len);
    config.ip = cJSON_IsString(ip) && ip->valuestring[0] ? parse_ip(ip->valuestring) : 0;
    if (cJSON_IsString(ip) && ip->valuestring[0] && !config.ip) {
        cJSON_Delete(body);
        return json_error(req, "400 Bad Request", "invalid AP IPv4 address");
    }
    config.enabled = cJSON_IsBool(enabled) ? cJSON_IsTrue(enabled) : true;
    cJSON_Delete(body);
    cJSON *reply = cJSON_CreateObject();
    cJSON_AddBoolToObject(reply, "ok", true);
    cJSON_AddBoolToObject(reply, "rebooting", true);
    send_json(req, reply);
    vTaskDelay(pdMS_TO_TICKS(250));
    return dc_wifi_set_ap_config_and_reboot(&config);
}

static esp_err_t product_post(httpd_req_t *req)
{
    if (require_auth(req) != ESP_OK) return ESP_OK;
    if (!s_config.apply_product)
        return json_error(req, "404 Not Found", "product setup is unavailable");
    cJSON *body = recv_json(req);
    cJSON *values = body ? cJSON_GetObjectItemCaseSensitive(body, "values") : NULL;
    if (!cJSON_IsObject(values)) {
        cJSON_Delete(body);
        return json_error(req, "400 Bad Request", "values object is required");
    }
    char message[128] = {0};
    esp_err_t err = s_config.apply_product(values, s_config.ctx, message, sizeof(message));
    cJSON_Delete(body);
    if (err != ESP_OK)
        return json_error(req, "400 Bad Request", message[0] ? message : esp_err_to_name(err));
    cJSON *reply = cJSON_CreateObject();
    cJSON_AddBoolToObject(reply, "ok", true);
    if (message[0]) cJSON_AddStringToObject(reply, "message", message);
    return send_json(req, reply);
}

static esp_err_t logs_get(httpd_req_t *req)
{
    if (require_auth(req) != ESP_OK) return ESP_OK;
    // The full snapshot is ~6.4 KiB. Keeping it on the HTTP task's 8 KiB stack
    // leaves too little room for cJSON and response handling and resets real
    // ESP32-C3 hardware with stack protection enabled.
    dc_evlog_entry_t *entries = calloc(DC_EVLOG_MAX_ENTRIES, sizeof(*entries));
    if (!entries)
        return json_error(req, "500 Internal Server Error", "out of memory");
    size_t count = dc_evlog_snapshot(entries, DC_EVLOG_MAX_ENTRIES);
    cJSON *root = cJSON_CreateObject();
    cJSON *logs = root ? cJSON_AddArrayToObject(root, "entries") : NULL;
    if (!root || !logs) {
        cJSON_Delete(root);
        free(entries);
        return json_error(req, "500 Internal Server Error", "out of memory");
    }
    for (size_t i = 0; i < count; ++i) {
        cJSON *entry = cJSON_CreateObject();
        if (!entry) {
            cJSON_Delete(root);
            free(entries);
            return json_error(req, "500 Internal Server Error", "out of memory");
        }
        cJSON_AddNumberToObject(entry, "ms", entries[i].ms);
        cJSON_AddStringToObject(entry, "text", entries[i].text);
        cJSON_AddItemToArray(logs, entry);
    }
    free(entries);
    return send_json(req, root);
}

// ---- family-shared firmware console page ------------------------------------
// Read-only /console served by core so every Dragon product gets it: streams the
// raw ESP_LOGx ring from dc_evlog (auth-gated data). Compact inline shell; the
// dashboard SPA is unaffected. Standalone route registered before the /* catch-all
// so it wins. (/diag is deliberately NOT here — diagnostics are device-specific,
// so each product owns its own /diag page.)

// Raw firmware console ring (auth-gated), the data source for /console.
static esp_err_t console_data_get(httpd_req_t *req)
{
    if (require_auth(req) != ESP_OK) return ESP_OK;
    char *buf = malloc(DC_EVLOG_CONSOLE_BYTES + 1);
    if (!buf) return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "out of memory");
    size_t n = dc_evlog_console_snapshot(buf, DC_EVLOG_CONSOLE_BYTES + 1);
    httpd_resp_set_type(req, "text/plain; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    esp_err_t err = httpd_resp_send(req, buf, n);
    free(buf);
    return err;
}

// Compact, theme-aware page shell for /console. Mirrors the
// dc_ui token names so the page matches the SPA in light and dark.
static const char PAGE_HEAD[] =
    "<!doctype html><html lang=en><head><meta charset=utf-8>"
    "<meta name=viewport content='width=device-width,initial-scale=1'>"
    "<meta name=color-scheme content='light dark'><meta name=referrer content=no-referrer>"
    "<link rel=icon href=/favicon.ico><style>"
    ":root{color-scheme:light dark;"
    "--text:light-dark(rgb(26 28 31),rgb(255 255 255));"
    "--bg:light-dark(rgb(255 255 255),rgb(24 24 24));"
    "--card:color-mix(in oklab,var(--text) 5%,transparent);"
    "--accent:light-dark(rgb(51 156 255),rgb(131 195 255));"
    "--accent-fg:light-dark(rgb(255 255 255),rgb(13 13 13));"
    "--muted:light-dark(rgb(26 28 31 / 49.4%),rgb(255 255 255 / 49.8%));"
    "--input:light-dark(rgb(26 28 31 / 11.8%),color-mix(in oklab,rgb(0 0 0) 10%,transparent));"
    "--border:light-dark(rgb(26 28 31 / 8%),rgb(255 255 255 / 8.2%));"
    "--bad:light-dark(rgb(226 85 7),rgb(255 133 73))}"
    ":root[data-theme=light]{color-scheme:light}:root[data-theme=dark]{color-scheme:dark}"
    "*{box-sizing:border-box}"
    "body{margin:0;background:var(--bg);color:var(--text);"
    "font:14px/1.4 -apple-system,system-ui,'Segoe UI',Roboto,sans-serif}"
    ".wrap{max-width:28em;margin:0 auto;padding:16px}"
    ".card{background:var(--card);border:1px solid var(--border);border-radius:12px;padding:16px;margin:14px 0}"
    ".card h2{margin:0 0 .3em;font-size:1rem;font-weight:600}"
    "button.go{width:100%;padding:13px;margin-top:8px;border:0;border-radius:9px;background:var(--accent);color:var(--accent-fg);font-size:1rem;font-weight:600;cursor:pointer}"
    "button.sec{width:100%;padding:10px;margin-top:12px;border:1px solid var(--border);border-radius:8px;background:transparent;color:var(--text);cursor:pointer}"
    ".warn{color:var(--bad);font-weight:700}"
    "small{color:var(--muted)}a{color:var(--accent)}</style>"
    "<script>var _t=localStorage.getItem('db_theme');"
    "if(_t==='light'||_t==='dark')document.documentElement.setAttribute('data-theme',_t);</script>"
    "</head><body><div class=wrap>";

static const char CONSOLE_BODY[] =
    "<style>"
    ".wrap{max-width:min(96vw,900px)}"
    "#c-log{font:12px/1.4 ui-monospace,Menlo,Consolas,monospace;white-space:pre;tab-size:4;"
    "background:var(--input);border-radius:8px;padding:10px 12px;"
    "max-height:70vh;overflow:auto;margin:.4em 0}"
    ".drow{display:flex;gap:8px}.drow button{flex:1;margin-top:0}"
    "</style>"
    "<div class=card><h2>Console</h2>"
    "<div id=c-meta><small>firmware log\xE2\x80\xA6</small></div>"
    "<pre id=c-log>loading\xE2\x80\xA6</pre>"
    "<div class=drow>"
    "<button type=button class=go id=c-dl>Download</button>"
    "<button type=button class=sec id=c-pause>Pause</button>"
    "</div></div>"
    "<p style='text-align:center'><small><a href='/'>\xE2\x86\x90 Back to status</a></small></p>"
    "<script>"
    // Presence-only auth needs a non-empty header; a real control token 403s
    // until entered. Default to a placeholder so unprotected devices never prompt.
    "function tok(){return localStorage.getItem('db_tok')||'web';}"
    "function hdr(){return {'X-DragonBreath-Auth':tok()};}"
    "(function(){"
    "var paused=false,last='',needtok=false;"
    "function $(i){return document.getElementById(i);}"
    "function load(){"
    "fetch('/api/v1/system/console',{cache:'no-store',headers:hdr()}).then(function(r){"
    "if(r.status==403){var e=prompt('Control token');"
    "if(e){localStorage.setItem('db_tok',e);needtok=false;load();}"
    "else{needtok=true;$('c-meta').innerHTML='<small class=warn>Control token required \\u2014 reload to enter it.</small>';}"
    "return null;}"
    "return r.text();"
    "}).then(function(t){"
    "if(t==null)return;t=t.replace(/\\x1b\\[[0-9;]*m/g,'');last=t;var pre=$('c-log');"
    "var atEnd=pre.scrollTop+pre.clientHeight>=pre.scrollHeight-6;"
    "pre.textContent=t||'(no log captured yet)';"
    "if(atEnd)pre.scrollTop=pre.scrollHeight;"
    "$('c-meta').innerHTML='<small>'+t.length+' bytes \\u00b7 '+(paused?'paused':'auto-refresh 2s')+'</small>';"
    "}).catch(function(){});"
    "}"
    "$('c-dl').addEventListener('click',function(){"
    "var b=new Blob([last||''],{type:'text/plain'});"
    "var a=document.createElement('a');a.href=URL.createObjectURL(b);a.download='dragon-console.txt';a.click();"
    "setTimeout(function(){URL.revokeObjectURL(a.href);},1000);"
    "});"
    "$('c-pause').addEventListener('click',function(){paused=!paused;this.textContent=paused?'Resume':'Pause';if(!paused)load();});"
    "load();setInterval(function(){if(!paused&&!needtok)load();},2000);"
    "})();</script></body></html>";

static esp_err_t console_page(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_send_chunk(req, PAGE_HEAD, HTTPD_RESP_USE_STRLEN);
    httpd_resp_send_chunk(req, CONSOLE_BODY, HTTPD_RESP_USE_STRLEN);
    return httpd_resp_send_chunk(req, NULL, 0);
}

static esp_err_t operation_error(httpd_req_t *req, esp_err_t err,
                                 const char *message)
{
    const char *status = err == ESP_ERR_INVALID_STATE ? "409 Conflict" :
                         err == ESP_ERR_INVALID_ARG ? "400 Bad Request" :
                         "500 Internal Server Error";
    return json_error(req, status, message && message[0] ? message : esp_err_to_name(err));
}

static esp_err_t guard_operation(httpd_req_t *req, dc_portal_operation_t operation)
{
    if (!s_config.guard_operation) return ESP_OK;
    char message[128] = {0};
    esp_err_t err = s_config.guard_operation(operation, s_config.ctx,
                                              message, sizeof(message));
    if (err == ESP_OK) return ESP_OK;
    operation_error(req, err, message);
    return err;
}

static esp_err_t update_post(httpd_req_t *req)
{
    if (require_auth(req) != ESP_OK) return ESP_OK;
    if (guard_operation(req, DC_PORTAL_OPERATION_OTA) != ESP_OK) return ESP_OK;
    if (req->content_len <= 0)
        return json_error(req, "400 Bad Request", "empty firmware upload");
    const esp_partition_t *partition = esp_ota_get_next_update_partition(NULL);
    if (!partition) return json_error(req, "500 Internal Server Error", "no OTA partition");
    esp_ota_handle_t update = 0;
    esp_err_t err = esp_ota_begin(partition, OTA_WITH_SEQUENTIAL_WRITES, &update);
    if (err != ESP_OK) return json_error(req, "500 Internal Server Error", esp_err_to_name(err));
    static uint8_t buffer[1024];
    mbedtls_sha256_context sha;
    mbedtls_sha256_init(&sha);
    mbedtls_sha256_starts(&sha, 0);
    int remaining = req->content_len;
    while (remaining > 0) {
        int got = httpd_req_recv(req, (char *)buffer,
                                 remaining < (int)sizeof(buffer) ? remaining : (int)sizeof(buffer));
        if (got == HTTPD_SOCK_ERR_TIMEOUT) continue;
        if (got <= 0 || esp_ota_write(update, buffer, (size_t)got) != ESP_OK) {
            mbedtls_sha256_free(&sha);
            esp_ota_abort(update);
            return json_error(req, "500 Internal Server Error", "OTA receive/write failed");
        }
        mbedtls_sha256_update(&sha, buffer, (size_t)got);
        remaining -= got;
    }
    uint8_t digest[32];
    mbedtls_sha256_finish(&sha, digest);
    mbedtls_sha256_free(&sha);
    err = esp_ota_end(update);
    if (err != ESP_OK) return json_error(req, "400 Bad Request", esp_err_to_name(err));
    esp_app_desc_t image = {0};
    err = esp_ota_get_partition_description(partition, &image);
    if (err != ESP_OK)
        return json_error(req, "400 Bad Request", "cannot read image descriptor");
    if (s_config.validate_image) {
        char message[128] = {0};
        err = s_config.validate_image(&image, s_config.ctx, message, sizeof(message));
        if (err != ESP_OK) {
            esp_err_t erase_err = esp_partition_erase_range(partition, 0,
                                                             partition->erase_size);
            if (erase_err != ESP_OK)
                ESP_LOGW(TAG, "could not invalidate rejected OTA image: %s",
                         esp_err_to_name(erase_err));
            return operation_error(req, err, message);
        }
    }
    err = esp_ota_set_boot_partition(partition);
    if (err != ESP_OK) return json_error(req, "500 Internal Server Error", esp_err_to_name(err));
    char sha_hex[65];
    for (size_t i = 0; i < sizeof(digest); ++i)
        snprintf(&sha_hex[i * 2], 3, "%02x", digest[i]);
    cJSON *reply = cJSON_CreateObject();
    cJSON_AddBoolToObject(reply, "ok", true);
    cJSON_AddBoolToObject(reply, "rebooting", true);
    cJSON_AddNumberToObject(reply, "bytes", req->content_len);
    cJSON_AddStringToObject(reply, "sha256", sha_hex);
    cJSON_AddStringToObject(reply, "project", image.project_name);
    cJSON_AddStringToObject(reply, "version", image.version);
    send_json(req, reply);
    vTaskDelay(pdMS_TO_TICKS(500));
    esp_restart();
    return ESP_OK; // unreachable
}

static bool reset_confirmed(httpd_req_t *req)
{
    char query[96] = {0}, value[32] = {0};
    return httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK &&
           httpd_query_key_value(query, "confirm", value, sizeof(value)) == ESP_OK &&
           strcmp(value, "factory-reset") == 0;
}

static esp_err_t reset_post(httpd_req_t *req)
{
    if (require_auth(req) != ESP_OK) return ESP_OK;
    if (!reset_confirmed(req))
        return json_error(req, "400 Bad Request", "factory reset requires confirm=factory-reset");
    if (guard_operation(req, DC_PORTAL_OPERATION_FACTORY_RESET) != ESP_OK) return ESP_OK;
    if (s_config.factory_reset) {
        esp_err_t err = s_config.factory_reset(s_config.ctx);
        if (err != ESP_OK) return json_error(req, "500 Internal Server Error", esp_err_to_name(err));
    }
    dc_wifi_clear_creds();
    cJSON *reply = cJSON_CreateObject();
    cJSON_AddBoolToObject(reply, "ok", true);
    cJSON_AddBoolToObject(reply, "rebooting", true);
    send_json(req, reply);
    vTaskDelay(pdMS_TO_TICKS(500));
    esp_restart();
    return ESP_OK; // unreachable
}

static uint32_t ap_gateway_ip(void)
{
    esp_netif_t *ap = esp_netif_get_handle_from_ifkey("WIFI_AP_DEF");
    esp_netif_ip_info_t info = {0};
    return ap && esp_netif_get_ip_info(ap, &info) == ESP_OK ? info.ip.addr : 0;
}

static esp_err_t register_handler(const httpd_uri_t *handler)
{
    esp_err_t err = httpd_register_uri_handler(s_httpd, handler);
    if (err != ESP_OK) ESP_LOGE(TAG, "register %s: %s", handler->uri, esp_err_to_name(err));
    return err;
}

esp_err_t dc_portal_start(const dc_portal_config_t *config)
{
    if (!config || !config->product || !config->display_name) return ESP_ERR_INVALID_ARG;
    if (s_httpd) return ESP_ERR_INVALID_STATE;
    s_config = *config;
    s_ap_mode = dc_wifi_state() == DC_WIFI_STATE_AP_PORTAL;
    httpd_config_t http = HTTPD_DEFAULT_CONFIG();
    if (config->httpd_config) http = *config->httpd_config;
    http.uri_match_fn = httpd_uri_match_wildcard;
    size_t minimum_handlers = 16 + config->product_route_count;
    if (http.max_uri_handlers < minimum_handlers)
        http.max_uri_handlers = minimum_handlers;
    // provisioning_get serializes the product descriptor AND the full Wi-Fi scan
    // list through cJSON's recursive printer, with the product's float fields
    // going via newlib's dtoa. Both are stack-hungry, and the 4096-byte
    // HTTPD_DEFAULT_CONFIG stack overflows once a handful of networks are in
    // range — faulting inside _Balloc or print_string_ptr rather than tripping
    // the stack canary, which is only checked on a context switch. Raised to a
    // floor rather than assigned, so a product supplying a larger stack in its
    // own httpd_config keeps it. Must stay AFTER the httpd_config copy above.
    if (http.stack_size < DC_PORTAL_MIN_HTTPD_STACK)
        http.stack_size = DC_PORTAL_MIN_HTTPD_STACK;
    esp_err_t err = httpd_start(&s_httpd, &http);
    if (err != ESP_OK) return err;

    const httpd_uri_t builtins[] = {
        { .uri = "/api/v1/provisioning", .method = HTTP_GET, .handler = provisioning_get },
        { .uri = "/api/v1/provisioning/scan", .method = HTTP_POST, .handler = scan_post },
        { .uri = "/api/v1/provisioning/wifi", .method = HTTP_POST, .handler = wifi_post },
        { .uri = "/api/v1/provisioning/ap", .method = HTTP_POST, .handler = ap_post },
        { .uri = "/api/v1/provisioning/product", .method = HTTP_POST, .handler = product_post },
        { .uri = "/api/v1/system/logs", .method = HTTP_GET, .handler = logs_get },
        { .uri = "/api/v1/system/console", .method = HTTP_GET, .handler = console_data_get },
        { .uri = "/api/v1/system/update", .method = HTTP_POST, .handler = update_post },
        // Compatibility alias used by existing DragonBreath/DragonVent clients.
        { .uri = "/update", .method = HTTP_POST, .handler = update_post },
        { .uri = "/api/v1/system/reset", .method = HTTP_POST, .handler = reset_post },
        { .uri = "/setup", .method = HTTP_GET, .handler = spa_get },
        { .uri = "/console", .method = HTTP_GET, .handler = console_page },
        { .uri = "/", .method = HTTP_GET, .handler = spa_get },
    };
    for (size_t i = 0; i < sizeof(builtins) / sizeof(builtins[0]); ++i) {
        err = register_handler(&builtins[i]);
        if (err != ESP_OK) goto fail;
    }
    for (size_t i = 0; i < config->product_route_count; ++i) {
        err = register_handler(&config->product_routes[i]);
        if (err != ESP_OK) goto fail;
    }
    if (config->register_product_routes) {
        err = config->register_product_routes(s_httpd, config->ctx);
        if (err != ESP_OK) goto fail;
    }
    const httpd_uri_t catchall = { .uri = "/*", .method = HTTP_GET, .handler = spa_get };
    err = register_handler(&catchall);
    if (err != ESP_OK) goto fail;
    dc_wifi_scan_start();
    if (s_ap_mode) {
        uint32_t ip = ap_gateway_ip();
        if (ip) dc_portal_dns_start(ip);
    }
    ESP_LOGI(TAG, "%s portal up (%s)", config->display_name, s_ap_mode ? "AP" : "STA");
    return ESP_OK;
fail:
    dc_portal_stop();
    return err;
}

esp_err_t dc_portal_stop(void)
{
    dc_portal_dns_stop();
    if (s_httpd) httpd_stop(s_httpd);
    s_httpd = NULL;
    memset(&s_config, 0, sizeof(s_config));
    return ESP_OK;
}

httpd_handle_t dc_portal_httpd(void)
{
    return s_httpd;
}
