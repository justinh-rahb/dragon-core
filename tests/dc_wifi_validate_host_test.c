#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "dc_wifi.h"

int main(void)
{
    char ssid32[33];
    memset(ssid32, 's', 32);
    ssid32[32] = '\0';
    char ssid33[34];
    memset(ssid33, 's', 33);
    ssid33[33] = '\0';
    assert(dc_wifi_ssid_valid(ssid32, false));
    assert(!dc_wifi_ssid_valid(ssid33, false));
    assert(!dc_wifi_ssid_valid("", false));
    assert(dc_wifi_ssid_valid("", true));

    assert(dc_wifi_password_valid(""));
    assert(!dc_wifi_password_valid("1234567"));
    assert(dc_wifi_password_valid("12345678"));

    char pass63[64];
    memset(pass63, 'p', 63);
    pass63[63] = '\0';
    assert(dc_wifi_password_valid(pass63));

    char hex64[65];
    memset(hex64, 'a', 64);
    hex64[64] = '\0';
    assert(dc_wifi_password_valid(hex64));
    hex64[10] = 'z';
    assert(!dc_wifi_password_valid(hex64));

    char pass65[66];
    memset(pass65, 'a', 65);
    pass65[65] = '\0';
    assert(!dc_wifi_password_valid(pass65));

    // Hostname (RFC 1123 label): 1-32 chars of [A-Za-z0-9-], no leading/trailing hyphen.
    assert(dc_wifi_hostname_valid("dragonbreath"));
    assert(dc_wifi_hostname_valid("chamber-1"));
    assert(dc_wifi_hostname_valid("A"));
    assert(dc_wifi_hostname_valid("123"));
    assert(!dc_wifi_hostname_valid(NULL));
    assert(!dc_wifi_hostname_valid(""));
    assert(!dc_wifi_hostname_valid("-lead"));
    assert(!dc_wifi_hostname_valid("trail-"));
    assert(!dc_wifi_hostname_valid("has space"));
    assert(!dc_wifi_hostname_valid("under_score"));
    assert(!dc_wifi_hostname_valid("dot.name"));
    char host32[33];
    memset(host32, 'h', 32);
    host32[32] = '\0';
    assert(dc_wifi_hostname_valid(host32));
    char host33[34];
    memset(host33, 'h', 33);
    host33[33] = '\0';
    assert(!dc_wifi_hostname_valid(host33));

    puts("dc_wifi credential validation: PASS");
    return 0;
}
