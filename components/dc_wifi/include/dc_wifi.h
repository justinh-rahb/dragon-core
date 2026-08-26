#pragma once

// Dragon family Wi-Fi manager: reads saved credentials from NVS at boot, connects
// as STA, and falls back to AP + captive portal if either the credentials are
// missing or the connection fails. Cred storage is compatible with the stock
// firmware's NVS layout (namespace "app_nvs", keys "ssid" / "password").

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
#include "esp_wifi_types.h"

typedef enum {
    DC_WIFI_STATE_INIT,
    DC_WIFI_STATE_STA_CONNECTING,
    DC_WIFI_STATE_STA_CONNECTED,
    DC_WIFI_STATE_AP_PORTAL,   // hosting captive portal for setup
} dc_wifi_state_t;

// Product identity supplied before dc_wifi_start(). Values are copied, so callers
// may use stack storage. The AP prefix is followed by four MAC-address hex digits.
typedef struct {
    const char *hostname;       // DNS-safe hostname, for example "dragonbreath"
    const char *instance_name;  // human-readable mDNS HTTP service instance
    const char *ap_ssid_prefix; // for example "DragonBreath_"
    const char *ap_password;    // empty for open; 8-63 chars or a 64-digit hex key
} dc_wifi_identity_t;

// Configure product-specific network identity. Must be called before dc_wifi_start().
// If omitted, family-neutral Dragon defaults are used.
esp_err_t dc_wifi_set_identity(const dc_wifi_identity_t *identity);

// AP hotspot lifecycle. The AP + captive portal is ALWAYS brought up when there
// are no saved STA credentials (first-boot setup is otherwise impossible); this
// mode governs the AP only once STA credentials exist. Unlike the old fallback
// model, ALWAYS/TEMP bring the AP up concurrently with STA (reachable even when
// Wi-Fi is working); the AP's own WPA2 password is its access gate.
typedef enum {
    DC_WIFI_AP_OFF    = 0,  // no concurrent AP (API-only; not offered in the UI).
                            //   STA-only. A wrong-credential STA failure leaves
                            //   no AP — recover with `flash.py --erase-nvs` or the
                            //   on-device Power+Auto reset combo.
    DC_WIFI_AP_ALWAYS = 1,  // AP stays up alongside STA, indefinitely.
    DC_WIFI_AP_TEMP   = 2,  // AP up for DC_WIFI_AP_TEMP_MINUTES after boot, then
                            //   dropped — unless STA never connected, in which
                            //   case the AP is kept as the recovery portal.
    DC_WIFI_AP_FALLBACK = 3, // STA-only while connected (no concurrent AP, so no
                            //   APSTA channel/power contention), but the recovery
                            //   portal IS brought up if STA fails to join. Best of
                            //   both: clean STA + no-USB recovery.
} dc_wifi_ap_mode_t;

// Minutes the DC_WIFI_AP_TEMP window stays open after boot.
#define DC_WIFI_AP_TEMP_MINUTES 15
// Default when nothing is stored: the setup/recovery AP is always reachable.
#define DC_WIFI_AP_MODE_DEFAULT DC_WIFI_AP_ALWAYS

// "off" / "always" / "temp" <-> enum, shared by the product portals so their
// wire formats never drift.
const char *dc_wifi_ap_mode_to_str(dc_wifi_ap_mode_t mode);
dc_wifi_ap_mode_t dc_wifi_ap_mode_from_str(const char *s, dc_wifi_ap_mode_t fallback);

// AP hotspot configuration, overridable via the portal. Empty strings and
// ip == 0 select the built-in defaults (MAC-derived SSID, "987654321",
// 192.168.4.1). Stored in NVS under app_nvs.
typedef struct {
    char              ssid[33];      // "" → configured product prefix + MAC suffix
    char              password[65];  // "" → default 987654321 (WPA2-PSK needs ≥ 8)
    uint32_t          ip;            // host byte order; 0 → default 192.168.4.1
    dc_wifi_ap_mode_t mode;          // see dc_wifi_ap_mode_t
} dc_wifi_ap_config_t;

#define DC_WIFI_SCAN_MAX 20

// Shared validation used by provisioning adapters and the persistence boundary.
// SSIDs are at most 32 bytes. Passwords are blank for an open network, 8-63
// characters, or exactly 64 hexadecimal digits for a raw WPA key.
bool dc_wifi_ssid_valid(const char *ssid, bool allow_empty);
bool dc_wifi_password_valid(const char *password);

// Start the WiFi manager. Non-blocking; state transitions happen async.
esp_err_t dc_wifi_start(void);

// Persist WiFi credentials and reboot into STA mode. Intended to be called
// from the captive-portal HTTP handler after the user submits the form.
esp_err_t dc_wifi_save_creds_and_reboot(const char *ssid, const char *password);

// Wipe saved WiFi credentials.
esp_err_t dc_wifi_clear_creds(void);

dc_wifi_state_t dc_wifi_state(void);

// If the last STA connection attempt this boot failed and the device fell back to
// the AP setup portal, fill ssid_out (the attempted SSID) and reason_out (a short,
// user-facing hint) and return true. Returns false if the last attempt succeeded
// or none was made. Either buffer may be NULL to skip it.
bool dc_wifi_last_sta_fail(char *ssid_out, size_t ssid_sz, char *reason_out, size_t reason_sz);

// Kick off an async scan of visible networks. Returns immediately; results
// land in the cache when WIFI_EVENT_SCAN_DONE fires. Safe to call in AP mode
// (driver runs in APSTA so the AP stays up).
esp_err_t dc_wifi_scan_start(void);

// True while a scan is in flight (results not yet populated for this cycle).
bool dc_wifi_is_scanning(void);

// Copy the latest cached scan results into `out` (capacity `max_count`).
// Returns the number of records actually written.
int dc_wifi_get_scan_results(wifi_ap_record_t *out, int max_count);

// AP hotspot config: current effective values (with defaults applied) and
// the setter (persists to NVS + reboots so netif re-inits with the new IP).
esp_err_t dc_wifi_get_ap_config(dc_wifi_ap_config_t *out);
esp_err_t dc_wifi_set_ap_config_and_reboot(const dc_wifi_ap_config_t *cfg);

// Family-neutral defaults used when a product does not supply an identity.
#define DC_WIFI_DEFAULT_AP_PASSWORD    "987654321"
#define DC_WIFI_DEFAULT_AP_SSID_PREFIX "Dragon_"
#define DC_WIFI_DEFAULT_HOSTNAME       "dragon"
#define DC_WIFI_DEFAULT_INSTANCE_NAME  "Dragon Device"
