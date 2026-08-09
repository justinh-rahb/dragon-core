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

    puts("dc_wifi credential validation: PASS");
    return 0;
}
