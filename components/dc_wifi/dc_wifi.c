#include "dc_wifi.h"

#include "esp_event.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "dhcpserver/dhcpserver.h"
#include "dhcpserver/dhcpserver_options.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "mdns.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "lwip/inet.h"

#include <stdlib.h>
#include <string.h>

static const char *TAG = "dc_wifi";

#define NVS_NS      "app_nvs"    // matches stock firmware
#define KEY_SSID    "ssid"
#define KEY_PASS    "password"
#define KEY_AP_SSID "ap_ssid"
#define KEY_AP_PASS "ap_pass"
#define KEY_AP_IP   "ap_ip"
#define KEY_AP_EN   "ap_enabled"

#define STA_MAX_RETRIES  5
#define BIT_CONNECTED    BIT0
#define BIT_FAILED       BIT1

#define DEFAULT_AP_IP  0xC0A80401U   // 192.168.4.1

static dc_wifi_state_t s_state = DC_WIFI_STATE_INIT;
static EventGroupHandle_t s_events = NULL;
static esp_netif_t *s_sta_netif = NULL;
static esp_netif_t *s_ap_netif = NULL;
static int s_retry = 0;
static bool s_mdns_started = false;
static char s_hostname[33] = DC_WIFI_DEFAULT_HOSTNAME;
static char s_instance_name[64] = DC_WIFI_DEFAULT_INSTANCE_NAME;
static char s_ap_ssid_prefix[29] = DC_WIFI_DEFAULT_AP_SSID_PREFIX;
static char s_ap_password[65] = DC_WIFI_DEFAULT_AP_PASSWORD;

// Last STA-attempt bookkeeping, so the AP setup page can explain a failed join
// instead of silently bouncing the user back to the portal. RAM-only: the AP
// fallback happens in the same boot as the failure, so no NVS persistence needed.
static char    s_sta_ssid[33]     = {0};    // SSID of the current/last STA attempt
static uint8_t s_last_disc_reason = 0;      // last STA_DISCONNECTED reason code
static bool    s_sta_failed       = false;  // last STA attempt failed -> AP fallback

// Scan cache — mutex-protected because on_wifi_event fires on the WiFi task
// but dc_wifi_get_scan_results is called from the httpd task.
static SemaphoreHandle_t s_scan_lock = NULL;
static wifi_ap_record_t s_scan_cache[DC_WIFI_SCAN_MAX];
static int              s_scan_count = 0;
static bool             s_scanning   = false;

static esp_err_t start_mdns(void)
{
    if (s_mdns_started) return ESP_OK;

    esp_err_t err = mdns_init();
    if (err != ESP_OK) return err;

    err = mdns_hostname_set(s_hostname);
    if (err != ESP_OK) return err;

    err = mdns_instance_name_set(s_instance_name);
    if (err != ESP_OK) return err;

    err = mdns_service_add(s_instance_name, "_http", "_tcp", 80, NULL, 0);
    if (err != ESP_OK) return err;

    s_mdns_started = true;
    ESP_LOGI(TAG, "mDNS hostname: %s.local", s_hostname);
    return ESP_OK;
}

// ---------- NVS helpers ----------

static esp_err_t nvs_read_str(const char *key, char *out, size_t out_sz)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NS, NVS_READONLY, &h);
    if (err != ESP_OK) return err;
    size_t len = out_sz;
    err = nvs_get_str(h, key, out, &len);
    nvs_close(h);
    return err;
}

static esp_err_t nvs_write_str(const char *key, const char *value)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;
    err = nvs_set_str(h, key, value);
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    return err;
}

// One-time stock-Panda NVS migration. When Dragon firmware is OTA-installed over
// stock (Panda 1.0.3/1.0.4) it inherits stock's `app_nvs`, where WiFi lives in a
// "wifi_info" blob and Moonraker in "moonraker_info" — different keys than ours. If our
// own keys are absent, lift the creds out of the stock blobs so the OTA carries WiFi +
// Moonraker across with NO re-provisioning. Runs once: writes our keys, then it's a
// no-op every subsequent boot (and never overwrites a user who set creds via /setup).
// Blob layouts RE'd from stock 1.0.4 NVS (confirmed with distinctive dummy values):
//   wifi_info[228]:      char ssid[33]@0, char password[64]@33, ap_ssid[33]@97, ...
//   moonraker_info[132]: char host[64]@0, uint32 port@64 (=printer :80, NOT :7125), name@68
//   ha_mqtt_info[146]:   char host[16]@0, char user[64]@16, char pass[64]@80, uint16 port@144
static void migrate_stock_nvs(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) return;
    size_t sz;

    if (nvs_get_str(h, KEY_SSID, NULL, &sz) == ESP_ERR_NVS_NOT_FOUND) {
        size_t blen = 0;
        if (nvs_get_blob(h, "wifi_info", NULL, &blen) == ESP_OK && blen >= 97) {
            uint8_t *b = calloc(1, blen);
            if (b && nvs_get_blob(h, "wifi_info", b, &blen) == ESP_OK) {
                char ssid[33] = {0}, pass[64] = {0};
                memcpy(ssid, b, 32);       // ssid[33]@0, keep the trailing NUL
                memcpy(pass, b + 33, 63);  // password[64]@33
                if (ssid[0]) {
                    nvs_set_str(h, KEY_SSID, ssid);
                    nvs_set_str(h, KEY_PASS, pass);
                    ESP_LOGW(TAG, "migrated stock WiFi (ssid '%s') from wifi_info blob", ssid);
                }
            }
            free(b);
        }
    }

    if (nvs_get_str(h, "mk_host", NULL, &sz) == ESP_ERR_NVS_NOT_FOUND) {
        size_t blen = 0;
        if (nvs_get_blob(h, "moonraker_info", NULL, &blen) == ESP_OK && blen >= 64) {
            uint8_t *b = calloc(1, blen);
            if (b && nvs_get_blob(h, "moonraker_info", b, &blen) == ESP_OK) {
                char host[64] = {0};
                memcpy(host, b, 63);   // host[64]@0
                if (host[0]) {
                    nvs_set_str(h, "mk_host", host);
                    nvs_set_u16(h, "mk_port", 7125);   // stock stores printer :80; DB wants Moonraker :7125
                    ESP_LOGW(TAG, "migrated stock Moonraker host '%s' (port->7125)", host);
                }
            }
            free(b);
        }
    }

    // Home Assistant MQTT: stock "ha_mqtt_info" blob (146 B):
    //   char host[16]@0, char user[64]@16, char pass[64]@80, uint16 port@144 (no topic).
    if (nvs_get_str(h, "ha_host", NULL, &sz) == ESP_ERR_NVS_NOT_FOUND) {
        size_t blen = 0;
        if (nvs_get_blob(h, "ha_mqtt_info", NULL, &blen) == ESP_OK && blen >= 146) {
            uint8_t *b = calloc(1, blen);
            if (b && nvs_get_blob(h, "ha_mqtt_info", b, &blen) == ESP_OK) {
                char host[16] = {0}, user[64] = {0}, pass[64] = {0};
                memcpy(host, b, 15);
                memcpy(user, b + 16, 63);
                memcpy(pass, b + 80, 63);
                uint16_t port = (uint16_t)(b[144] | (b[145] << 8));
                if (host[0]) {
                    nvs_set_str(h, "ha_host", host);
                    nvs_set_str(h, "ha_user", user);
                    nvs_set_str(h, "ha_pass", pass);
                    if (port) nvs_set_u16(h, "ha_port", port);
                    ESP_LOGW(TAG, "migrated stock HA MQTT broker '%s' from ha_mqtt_info", host);
                }
            }
            free(b);
        }
    }
    // Bambu Lab LAN MQTT: stock "bambu_mqtt_info" blob (169 B). Layout RE'd by binding
    // a stock 1.0.4 Panda to a bambuddy virtual X1C and dumping NVS:
    //   char ip[16]@0, char access_code[9]@16, char serial[16]@25, char name[128]@41.
    // (stock only persists this once a live printer bind succeeds — bambuddy provided
    // the printer.) Map ip->bb_host, serial->bb_serial, access_code->bb_code.
    if (nvs_get_str(h, "bb_host", NULL, &sz) == ESP_ERR_NVS_NOT_FOUND) {
        size_t blen = 0;
        if (nvs_get_blob(h, "bambu_mqtt_info", NULL, &blen) == ESP_OK && blen >= 41) {
            uint8_t *b = calloc(1, blen);
            if (b && nvs_get_blob(h, "bambu_mqtt_info", b, &blen) == ESP_OK) {
                char ip[16] = {0}, code[16] = {0}, serial[16] = {0};
                memcpy(ip,     b,      15);   // ip[16]@0
                memcpy(code,   b + 16,  8);   // access_code[9]@16 (8-digit code)
                memcpy(serial, b + 25, 15);   // serial[16]@25 (15-char serial)
                if (ip[0] && serial[0]) {
                    nvs_set_str(h, "bb_host", ip);
                    nvs_set_str(h, "bb_serial", serial);
                    nvs_set_str(h, "bb_code", code);
                    ESP_LOGW(TAG, "migrated stock Bambu printer (serial '%s' @ %s) from bambu_mqtt_info", serial, ip);
                }
            }
            free(b);
        }
    }

    nvs_commit(h);
    nvs_close(h);
}

static bool load_saved_creds(char *ssid, size_t ssid_sz, char *pass, size_t pass_sz)
{
    esp_err_t err = nvs_read_str(KEY_SSID, ssid, ssid_sz);
    if (err != ESP_OK || ssid[0] == '\0') return false;
    err = nvs_read_str(KEY_PASS, pass, pass_sz);
    if (err != ESP_OK) pass[0] = '\0';  // open network is legal
    return true;
}

// ---------- AP mode ----------

static void build_default_ap_ssid(char *out, size_t out_sz)
{
    uint8_t mac[6];
    esp_wifi_get_mac(WIFI_IF_AP, mac);
    snprintf(out, out_sz, "%s%02X%02X", s_ap_ssid_prefix, mac[4], mac[5]);
}

// Populate `out` with the effective AP config: NVS values if set, defaults
// otherwise (MAC-derived SSID, hardcoded password, 192.168.4.1).
static void load_ap_config(dc_wifi_ap_config_t *out)
{
    memset(out, 0, sizeof(*out));

    if (nvs_read_str(KEY_AP_SSID, out->ssid, sizeof(out->ssid)) != ESP_OK ||
        out->ssid[0] == '\0') {
        build_default_ap_ssid(out->ssid, sizeof(out->ssid));
    }
    if (nvs_read_str(KEY_AP_PASS, out->password, sizeof(out->password)) != ESP_OK ||
        out->password[0] == '\0') {
        strncpy(out->password, s_ap_password, sizeof(out->password) - 1);
    }

    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) == ESP_OK) {
        uint32_t ip = 0;
        if (nvs_get_u32(h, KEY_AP_IP, &ip) != ESP_OK || ip == 0) ip = DEFAULT_AP_IP;
        out->ip = ip;
        uint8_t en = 1;
        if (nvs_get_u8(h, KEY_AP_EN, &en) != ESP_OK) en = 1;   // default on
        out->enabled = (en != 0);
        nvs_close(h);
    } else {
        out->ip = DEFAULT_AP_IP;
        out->enabled = true;
    }
}

// Reassign the AP netif's IP + DHCP pool. Must happen while the AP is down.
static esp_err_t apply_ap_ip(uint32_t ip_host_order)
{
    if (s_ap_netif == NULL) return ESP_ERR_INVALID_STATE;
    ESP_ERROR_CHECK(esp_netif_dhcps_stop(s_ap_netif));

    esp_netif_ip_info_t info = {
        .ip.addr      = htonl(ip_host_order),
        .netmask.addr = htonl(0xFFFFFF00U),   // /24
        .gw.addr      = htonl(ip_host_order),
    };
    ESP_ERROR_CHECK(esp_netif_set_ip_info(s_ap_netif, &info));

    // Advertise ourselves as the DNS server via DHCP option 6. Without this,
    // clients that joined never learn where our fake DNS is, iOS/Android
    // captive-portal probes go to their default DNS (unreachable from our
    // subnet), and the "Sign in to network" banner never fires.
    dhcps_offer_t offer_dns = OFFER_DNS;
    ESP_ERROR_CHECK(esp_netif_dhcps_option(
        s_ap_netif, ESP_NETIF_OP_SET, ESP_NETIF_DOMAIN_NAME_SERVER,
        &offer_dns, sizeof(offer_dns)));
    esp_netif_dns_info_t dns = {
        .ip = { .u_addr.ip4.addr = info.ip.addr, .type = IPADDR_TYPE_V4 },
    };
    ESP_ERROR_CHECK(esp_netif_set_dns_info(s_ap_netif, ESP_NETIF_DNS_MAIN, &dns));

    ESP_ERROR_CHECK(esp_netif_dhcps_start(s_ap_netif));
    return ESP_OK;
}

static void start_ap_mode(void)
{
    ESP_LOGI(TAG, "starting AP + captive portal");
    // Flip state first so any STA disconnect events firing during the driver
    // restart don't fall into the retry branch and re-issue esp_wifi_connect.
    s_state = DC_WIFI_STATE_AP_PORTAL;
    ESP_ERROR_CHECK(esp_wifi_stop());

    dc_wifi_ap_config_t cfg;
    load_ap_config(&cfg);
    apply_ap_ip(cfg.ip);

    wifi_config_t ap = {0};
    size_t ssid_len = strlen(cfg.ssid);
    size_t password_len = strlen(cfg.password);
    memcpy(ap.ap.ssid, cfg.ssid, ssid_len);
    memcpy(ap.ap.password, cfg.password, password_len);
    ap.ap.ssid_len       = ssid_len;
    ap.ap.channel        = 1;
    ap.ap.max_connection = 4;
    // Open network is legal too, but WPA2 requires ≥ 8 chars for the password.
    ap.ap.authmode = strlen(cfg.password) >= 8 ? WIFI_AUTH_WPA2_PSK
                                               : WIFI_AUTH_OPEN;

    // APSTA so dc_wifi_scan_start() can enumerate networks without dropping
    // the portal AP off the air.
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap));
    ESP_ERROR_CHECK(esp_wifi_start());

    s_state = DC_WIFI_STATE_AP_PORTAL;
    // uint32_t is 'long unsigned' under IDF 5.3's toolchain — cast per octet
    // so %u picks up plain unsigned int.
    ESP_LOGI(TAG, "AP SSID=%s ip=%u.%u.%u.%u",
             cfg.ssid,
             (unsigned)((cfg.ip >> 24) & 0xFF),
             (unsigned)((cfg.ip >> 16) & 0xFF),
             (unsigned)((cfg.ip >>  8) & 0xFF),
             (unsigned)( cfg.ip        & 0xFF));
    // Portal is started by app_main after dc_wifi_start returns.
}

// ---------- STA mode ----------

static void start_sta_mode(const char *ssid, const char *pass)
{
    ESP_LOGI(TAG, "connecting to %s", ssid);
    snprintf(s_sta_ssid, sizeof s_sta_ssid, "%s", ssid);   // remember for a failed-join message

    wifi_config_t sta = {0};
    memcpy(sta.sta.ssid, ssid, strlen(ssid));
    memcpy(sta.sta.password, pass, strlen(pass));

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &sta));
    s_state = DC_WIFI_STATE_STA_CONNECTING;
    ESP_ERROR_CHECK(esp_wifi_start());
}

// ---------- Event handlers ----------

static void handle_scan_done(void)
{
    // Land the results directly in the cache — the event-loop task's stack
    // (default 2304 B) can't spare 1.6 KB for a local wifi_ap_record_t[20].
    uint16_t count = DC_WIFI_SCAN_MAX;
    xSemaphoreTake(s_scan_lock, portMAX_DELAY);
    esp_err_t err = esp_wifi_scan_get_ap_records(&count, s_scan_cache);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "scan_get_ap_records: %s", esp_err_to_name(err));
        count = 0;
    }
    s_scan_count = count;
    s_scanning = false;
    xSemaphoreGive(s_scan_lock);
    ESP_LOGI(TAG, "scan done: %d networks", (int)count);
}

static void on_wifi_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base != WIFI_EVENT) return;

    if (id == WIFI_EVENT_STA_START) {
        // Only auto-connect if we're actually trying to be a station.
        if (s_state == DC_WIFI_STATE_STA_CONNECTING) esp_wifi_connect();
    } else if (id == WIFI_EVENT_STA_DISCONNECTED) {
        const wifi_event_sta_disconnected_t *d = (const wifi_event_sta_disconnected_t *)data;
        if (d) s_last_disc_reason = d->reason;   // keep the latest reason for the setup page
        if (s_state == DC_WIFI_STATE_STA_CONNECTING || s_state == DC_WIFI_STATE_STA_CONNECTED) {
            if (s_retry < STA_MAX_RETRIES) {
                s_retry++;
                ESP_LOGW(TAG, "STA disconnect; retry %d/%d", s_retry, STA_MAX_RETRIES);
                esp_wifi_connect();
            } else {
                ESP_LOGW(TAG, "STA gave up; falling back to portal");
                xEventGroupSetBits(s_events, BIT_FAILED);
            }
        }
    } else if (id == WIFI_EVENT_SCAN_DONE) {
        handle_scan_done();
    }
}

static void on_ip_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *evt = (ip_event_got_ip_t *)data;
        ESP_LOGI(TAG, "got IP " IPSTR, IP2STR(&evt->ip_info.ip));
        s_retry = 0;
        s_state = DC_WIFI_STATE_STA_CONNECTED;
        s_sta_failed = false;
        xEventGroupSetBits(s_events, BIT_CONNECTED);
    }
}

// Map an ESP-IDF STA disconnect reason to a short, user-actionable hint. Returns
// NULL for reasons without a specific hint (caller shows the raw code instead).
static const char *sta_disc_reason_hint(uint8_t reason)
{
    switch (reason) {
    case WIFI_REASON_NO_AP_FOUND:
        return "network not found \xE2\x80\x94 check the name; the heater is 2.4 GHz only "
               "(5 GHz networks won't work)";
    case WIFI_REASON_AUTH_FAIL:
    case WIFI_REASON_AUTH_EXPIRE:
    case WIFI_REASON_HANDSHAKE_TIMEOUT:
    case WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT:
        return "wrong Wi-Fi password";
    case WIFI_REASON_BEACON_TIMEOUT:
    case WIFI_REASON_ASSOC_EXPIRE:
        return "weak signal \xE2\x80\x94 move the device closer to the router";
    default:
        return NULL;
    }
}

bool dc_wifi_last_sta_fail(char *ssid_out, size_t ssid_sz, char *reason_out, size_t reason_sz)
{
    if (!s_sta_failed) return false;
    if (ssid_out && ssid_sz)
        snprintf(ssid_out, ssid_sz, "%s", s_sta_ssid);
    if (reason_out && reason_sz) {
        const char *hint = sta_disc_reason_hint(s_last_disc_reason);
        if (hint) snprintf(reason_out, reason_sz, "%s", hint);
        else      snprintf(reason_out, reason_sz, "couldn't connect (reason %u)",
                           (unsigned)s_last_disc_reason);
    }
    return true;
}

// ---------- Public API ----------

esp_err_t dc_wifi_set_identity(const dc_wifi_identity_t *identity)
{
    if (identity == NULL || identity->hostname == NULL || identity->hostname[0] == '\0' ||
        identity->instance_name == NULL || identity->instance_name[0] == '\0' ||
        identity->ap_ssid_prefix == NULL || identity->ap_ssid_prefix[0] == '\0' ||
        identity->ap_password == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_state != DC_WIFI_STATE_INIT || s_mdns_started || s_sta_netif != NULL || s_ap_netif != NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    size_t hostname_len = strlen(identity->hostname);
    size_t instance_len = strlen(identity->instance_name);
    size_t prefix_len = strlen(identity->ap_ssid_prefix);
    size_t password_len = strlen(identity->ap_password);
    if (hostname_len >= sizeof(s_hostname) || instance_len >= sizeof(s_instance_name) ||
        prefix_len >= sizeof(s_ap_ssid_prefix) || password_len >= sizeof(s_ap_password) ||
        !dc_wifi_password_valid(identity->ap_password)) {
        return ESP_ERR_INVALID_ARG;
    }
    snprintf(s_hostname, sizeof(s_hostname), "%s", identity->hostname);
    snprintf(s_instance_name, sizeof(s_instance_name), "%s", identity->instance_name);
    snprintf(s_ap_ssid_prefix, sizeof(s_ap_ssid_prefix), "%s", identity->ap_ssid_prefix);
    snprintf(s_ap_password, sizeof(s_ap_password), "%s", identity->ap_password);
    return ESP_OK;
}

esp_err_t dc_wifi_start(void)
{
    // NVS
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ESP_ERROR_CHECK(nvs_flash_init());
    } else if (err != ESP_OK) {
        return err;
    }

    // First boot after an OTA-over-stock: carry WiFi + Moonraker across from stock's
    // app_nvs blobs so the device rejoins without re-provisioning.
    migrate_stock_nvs();

    // Netif + event loop
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    s_sta_netif = esp_netif_create_default_wifi_sta();
    s_ap_netif = esp_netif_create_default_wifi_ap();
    if (s_sta_netif == NULL || s_ap_netif == NULL) return ESP_FAIL;

    ESP_ERROR_CHECK(esp_netif_set_hostname(s_sta_netif, s_hostname));
    ESP_ERROR_CHECK(esp_netif_set_hostname(s_ap_netif, s_hostname));
    ESP_ERROR_CHECK(start_mdns());

    // WiFi driver
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));

    s_events = xEventGroupCreate();
    s_scan_lock = xSemaphoreCreateMutex();
    if (s_scan_lock == NULL) return ESP_ERR_NO_MEM;
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID, on_wifi_event, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        IP_EVENT, IP_EVENT_STA_GOT_IP, on_ip_event, NULL, NULL));

    char ssid[33] = {0};
    char pass[65] = {0};
    if (load_saved_creds(ssid, sizeof(ssid), pass, sizeof(pass))) {
        start_sta_mode(ssid, pass);
        // Wait for a decision. STA_MAX_RETRIES * ~4 s each ≈ 20 s of real work,
        // plus a generous margin so a slow-associating AP still has room.
        EventBits_t bits = xEventGroupWaitBits(
            s_events, BIT_CONNECTED | BIT_FAILED, pdFALSE, pdFALSE,
            pdMS_TO_TICKS(30000));
        if (!(bits & BIT_CONNECTED)) {
            dc_wifi_ap_config_t ap_cfg;
            load_ap_config(&ap_cfg);
            if (ap_cfg.enabled) {
                s_sta_failed = true;   // let the setup page explain why the join failed
                ESP_LOGW(TAG, "STA never came up (bits=0x%x) — falling back to AP",
                         (unsigned)bits);
                start_ap_mode();
            } else {
                ESP_LOGW(TAG, "STA never came up; AP fallback is disabled — "
                              "letting the driver keep retrying in the background");
                // Leave s_state == STA_CONNECTING; the wifi driver will keep
                // its own auto-reconnect running.
            }
        }
    } else {
        ESP_LOGI(TAG, "no saved WiFi credentials");
        start_ap_mode();
    }
    return ESP_OK;
}

esp_err_t dc_wifi_save_creds_and_reboot(const char *ssid, const char *password)
{
    if (!dc_wifi_ssid_valid(ssid, false) ||
        !dc_wifi_password_valid(password)) return ESP_ERR_INVALID_ARG;
    ESP_LOGI(TAG, "saving creds for SSID=%s; rebooting", ssid);
    esp_err_t err = nvs_write_str(KEY_SSID, ssid);
    if (err == ESP_OK) err = nvs_write_str(KEY_PASS, password ? password : "");
    if (err != ESP_OK) return err;

    // Give the HTTP response a moment to flush before we reboot.
    vTaskDelay(pdMS_TO_TICKS(500));
    esp_restart();
    return ESP_OK;   // unreachable
}

esp_err_t dc_wifi_clear_creds(void)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;
    nvs_erase_key(h, KEY_SSID);
    nvs_erase_key(h, KEY_PASS);
    nvs_commit(h);
    nvs_close(h);
    return ESP_OK;
}

dc_wifi_state_t dc_wifi_state(void) { return s_state; }

// ---------- Scan ----------

esp_err_t dc_wifi_scan_start(void)
{
    if (s_scan_lock == NULL) return ESP_ERR_INVALID_STATE;
    xSemaphoreTake(s_scan_lock, portMAX_DELAY);
    if (s_scanning) {
        xSemaphoreGive(s_scan_lock);
        return ESP_OK;   // scan already in flight — coalesce
    }
    s_scanning = true;
    xSemaphoreGive(s_scan_lock);

    wifi_scan_config_t cfg = {0};   // all channels, active scan, no ssid filter
    esp_err_t err = esp_wifi_scan_start(&cfg, false);
    if (err != ESP_OK) {
        xSemaphoreTake(s_scan_lock, portMAX_DELAY);
        s_scanning = false;
        xSemaphoreGive(s_scan_lock);
        ESP_LOGW(TAG, "scan_start: %s", esp_err_to_name(err));
    }
    return err;
}

bool dc_wifi_is_scanning(void)
{
    if (s_scan_lock == NULL) return false;
    bool r;
    xSemaphoreTake(s_scan_lock, portMAX_DELAY);
    r = s_scanning;
    xSemaphoreGive(s_scan_lock);
    return r;
}

int dc_wifi_get_scan_results(wifi_ap_record_t *out, int max_count)
{
    if (out == NULL || max_count <= 0 || s_scan_lock == NULL) return 0;
    int n;
    xSemaphoreTake(s_scan_lock, portMAX_DELAY);
    n = s_scan_count < max_count ? s_scan_count : max_count;
    memcpy(out, s_scan_cache, n * sizeof(wifi_ap_record_t));
    xSemaphoreGive(s_scan_lock);
    return n;
}

// ---------- AP config ----------

esp_err_t dc_wifi_get_ap_config(dc_wifi_ap_config_t *out)
{
    if (out == NULL) return ESP_ERR_INVALID_ARG;
    load_ap_config(out);
    return ESP_OK;
}

esp_err_t dc_wifi_set_ap_config_and_reboot(const dc_wifi_ap_config_t *cfg)
{
    if (cfg == NULL || !dc_wifi_ssid_valid(cfg->ssid, true) ||
        !dc_wifi_password_valid(cfg->password)) return ESP_ERR_INVALID_ARG;

    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;
    // Empty string clears the entry so the default reapplies.
    if (cfg->ssid[0] == '\0') nvs_erase_key(h, KEY_AP_SSID);
    else                      nvs_set_str(h, KEY_AP_SSID, cfg->ssid);
    if (cfg->password[0] == '\0') nvs_erase_key(h, KEY_AP_PASS);
    else                          nvs_set_str(h, KEY_AP_PASS, cfg->password);
    if (cfg->ip == 0) nvs_erase_key(h, KEY_AP_IP);
    else              nvs_set_u32(h, KEY_AP_IP, cfg->ip);
    nvs_set_u8(h, KEY_AP_EN, cfg->enabled ? 1 : 0);
    err = nvs_commit(h);
    nvs_close(h);
    if (err != ESP_OK) return err;

    ESP_LOGI(TAG, "AP config saved; rebooting");
    vTaskDelay(pdMS_TO_TICKS(500));
    esp_restart();
    return ESP_OK;   // unreachable
}
