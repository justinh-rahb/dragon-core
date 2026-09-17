// SPDX-License-Identifier: MIT
#include "dc_wifi.h"

#include <ctype.h>
#include <string.h>

bool dc_wifi_ssid_valid(const char *ssid, bool allow_empty)
{
    if (ssid == NULL) return false;
    size_t len = strlen(ssid);
    return (allow_empty || len > 0) && len <= 32;
}

bool dc_wifi_password_valid(const char *password)
{
    if (password == NULL) return true;
    size_t len = strlen(password);
    if (len == 0 || (len >= 8 && len <= 63)) return true;
    if (len != 64) return false;
    for (size_t i = 0; i < len; ++i) {
        if (!isxdigit((unsigned char)password[i])) return false;
    }
    return true;
}

bool dc_wifi_hostname_valid(const char *hostname)
{
    // A single DNS label (RFC 1123): 1-32 chars, letters/digits/hyphen, and no
    // leading or trailing hyphen. The 32-char cap matches the identity buffer.
    // esp_netif/mDNS accept a wider range, so this is what actually keeps a bad
    // value from silently breaking DHCP/mDNS resolution on the LAN.
    if (hostname == NULL) return false;
    size_t len = strlen(hostname);
    if (len == 0 || len > 32) return false;
    if (hostname[0] == '-' || hostname[len - 1] == '-') return false;
    for (size_t i = 0; i < len; ++i) {
        char c = hostname[i];
        if (!isalnum((unsigned char)c) && c != '-') return false;
    }
    return true;
}
