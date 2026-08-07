#pragma once
// Control-source selector. The device binds to EXACTLY ONE printer/controller at
// a time (never several). Klipper (Moonraker) is the default and the shipped
// path; Bambu and Home Assistant are optional parity sources. The choice is
// persisted in NVS (app_nvs / "ctl_src") and read once at boot by app_main, which
// starts only the selected client. See plans/control-source-bambu-ha.md.
#include "esp_err.h"
#include <stdint.h>

typedef enum {
    DC_SRC_KLIPPER = 0,   // Moonraker WebSocket — default, the real target
    DC_SRC_BAMBU   = 1,   // Bambu LAN MQTT bed-follow (read-only)
    DC_SRC_HA      = 2,   // Home Assistant MQTT — HA is the controller
    DC_SRC_NONE    = 3,   // unbound — no external controller (web/manual only)
} dc_ctl_source_t;

// Persisted control source. Returns DC_SRC_KLIPPER if unset or out of range
// (fail-safe to the shipped path).
dc_ctl_source_t dc_source_get(void);

// Persist the control source. Takes effect on the next boot (app_main starts the
// selected client at bring-up).
esp_err_t dc_source_set(dc_ctl_source_t src);

const char *dc_source_str(dc_ctl_source_t src);
