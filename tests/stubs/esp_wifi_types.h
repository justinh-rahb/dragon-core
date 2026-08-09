#pragma once

#include <stdint.h>

typedef struct {
    uint8_t ssid[33];
    int8_t rssi;
} wifi_ap_record_t;
