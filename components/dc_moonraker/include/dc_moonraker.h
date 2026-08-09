#pragma once

// Moonraker WebSocket client. Reads its config from NVS at boot, connects to
// `ws://<host>:<port>/websocket`, subscribes to a fixed set of printer objects,
// and caches the latest values for the vent policy to consult. Reconnects on
// disconnect. Idle (no-op) if no config is saved.

#include <stdbool.h>
#include "esp_err.h"

typedef enum {
    DC_MK_DISABLED,       // no config saved
    DC_MK_DISCONNECTED,   // config present, not currently connected
    DC_MK_CONNECTING,
    DC_MK_CONNECTED,      // websocket up, subscribe in flight
    DC_MK_SUBSCRIBED,     // receiving status updates
} dc_moonraker_state_t;

// Six-state printer model mirroring what stock reads off Bambu MQTT. Derived
// from a combination of print_stats.state and webhooks.state so a hard
// Klipper error shows as ERROR rather than a stale "printing".
typedef enum {
    DC_PRINTER_UNKNOWN = 0,
    DC_PRINTER_IDLE,       // standby / ready, no active print
    DC_PRINTER_PREPARING,  // print started but early (progress < ~1%)
    DC_PRINTER_PRINTING,   // actively printing
    DC_PRINTER_PAUSED,
    DC_PRINTER_COMPLETE,   // last print finished cleanly
    DC_PRINTER_ERROR,      // print_stats "error"/"cancelled", or Klipper shutdown
} dc_printer_state_t;

const char *dc_printer_state_str(dc_printer_state_t s);

typedef struct {
    char     host[64];    // hostname or IP; empty string = unconfigured
    uint16_t port;        // defaults to 7125 if 0
    char     api_key[65]; // optional; empty if unused
} dc_moonraker_config_t;

typedef struct {
    dc_moonraker_state_t state;
    dc_printer_state_t   printer;      // six-state model
    bool                 printing;     // convenience: printer == PRINTING
    float                bed_temp;     // heater_bed.temperature (°C)
    float                bed_target;   // heater_bed.target (°C)
    float                extruder_temp;// extruder.temperature (°C, informational)
    float                chamber_temp; // heater_generic chamber, if present; NaN if absent
    float                progress;     // 0..1 from virtual_sdcard.progress
    char                 filename[64]; // print_stats.filename
    char                 material[16]; // from save_variables.material or gcode metadata; upper-case
} dc_moonraker_status_t;

esp_err_t dc_moonraker_start(void);

// Overwrite the saved config. Safe before dc_moonraker_start(); if the client is
// running, it reconnects with the new settings.
esp_err_t dc_moonraker_set_config(const dc_moonraker_config_t *cfg);

// Returns persisted config even when dc_moonraker_start() has not been called.
esp_err_t dc_moonraker_get_config(dc_moonraker_config_t *out);
esp_err_t dc_moonraker_get_status(dc_moonraker_status_t *out);

// Wipe saved Moonraker config. Used for factory reset.
esp_err_t dc_moonraker_clear_config(void);
