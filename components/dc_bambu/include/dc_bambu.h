#pragma once
// Bambu Lab LAN MQTT client. Connects to the printer's on-device broker in LAN
// mode (mqtts://<host>:8883, username "bblp", password = LAN access code; the
// self-signed cert has CN=serial while we connect by IP, so cert verification is
// relaxed — LAN read-only), subscribes device/<serial>/report, publishes one
// "pushall" on connect (required on P1/A1 which send deltas), and scans each
// report for bed_temper / chamber_temper. The cached bed temperature feeds the
// AUTO seam exactly as Moonraker does. Read-only: we never send control commands
// to the printer. UNTESTED against real hardware — for community validation (see
// plans/control-source-bambu-ha.md).
#include <stdbool.h>
#include "esp_err.h"

typedef enum {
    DC_BAMBU_DISABLED,      // no config saved / source not selected
    DC_BAMBU_DISCONNECTED,  // config present, not currently connected
    DC_BAMBU_CONNECTING,
    DC_BAMBU_CONNECTED,     // MQTT+TLS session up, subscribe in flight
    DC_BAMBU_SUBSCRIBED,    // receiving report updates
} dc_bambu_state_t;

typedef struct {
    char host[64];    // printer IP/hostname; empty string = unconfigured
    char serial[32];  // printer serial (embedded in the MQTT topic path)
    char code[32];    // LAN access code (MQTT password)
} dc_bambu_config_t;

typedef struct {
    dc_bambu_state_t state;
    bool  connected;      // convenience: state == DC_BAMBU_SUBSCRIBED
    float bed_temp;       // bed_temper (°C); NaN until first report
    float bed_target;     // bed_target_temper (°C, the setpoint AUTO triggers on)
    float chamber_temp;   // chamber_temper (°C); NaN if the model has no sensor
    char  filament[16];   // active filament type from AMS / ext spool (e.g. "PETG");
                          // "" if unknown. Feeds filament-based chamber zones.
    bool  printing;       // gcode_state is PREPARE/RUNNING/PAUSE (a print is active);
                          // gates when a filament zone is applied.
    bool  error;          // gcode_state is FAILED (print failed / errored)
} dc_bambu_status_t;

esp_err_t dc_bambu_start(void);

// Overwrite saved config (NVS). Safe before dc_bambu_start(); the running client
// (once implemented) will reconnect with the new settings.
esp_err_t dc_bambu_set_config(const dc_bambu_config_t *cfg);

// Returns persisted config even when dc_bambu_start() has not been called.
esp_err_t dc_bambu_get_config(dc_bambu_config_t *out);
esp_err_t dc_bambu_get_status(dc_bambu_status_t *out);

// Wipe saved Bambu config (factory reset).
esp_err_t dc_bambu_clear_config(void);

// --- Filament chamber zones (Bambu only, issue #64) -------------------------
// Maps the active filament type to a chamber target so a Bambu print gets a warm
// chamber (e.g. PETG -> 40 C) without any bed-threshold AUTO. Klipper doesn't use
// this — it drives the chamber via M141/M191. A zone target of 0 = "no zone" (off).
//
// There are 6 BUILT-IN filament types (fixed defaults, editable target) plus up to
// DC_BAMBU_CUSTOM_MAX USER profiles the operator can add/remove for filaments not in
// the built-in set (PA, PCTG, ...). get_all returns the built-ins first, then customs.
#define DC_BAMBU_ZONE_COUNT  6                                        // built-in types
#define DC_BAMBU_CUSTOM_MAX  8                                        // user profiles
#define DC_BAMBU_ZONE_MAX    (DC_BAMBU_ZONE_COUNT + DC_BAMBU_CUSTOM_MAX)

typedef struct {
    char    name[12];    // filament type ("PETG", or a custom name like "PCTG")
    uint8_t target_c;    // chamber target (°C); 0 = no zone / off
    uint8_t default_c;   // built-in default (for the UI's "default N" hint); 0 for customs
    bool    custom;      // true = a user-added profile (removable)
} dc_bambu_zone_t;

// Resolve the chamber target for a filament type string. Longest case-insensitive
// prefix match over built-ins + customs (so a custom "PETG-CF" beats "PETG"). 0 = none.
uint8_t dc_bambu_zone_target(const char *filament);

// Fill `out` (capacity `max`) with built-in zones then custom profiles; returns the
// number written (<= DC_BAMBU_ZONE_MAX).
int dc_bambu_zone_get_all(dc_bambu_zone_t *out, int max);

// Set/update a zone by name (case-insensitive). A built-in name updates its target;
// an unknown name ADDS a custom profile (up to DC_BAMBU_CUSTOM_MAX). Persists.
esp_err_t dc_bambu_zone_set(const char *name, uint8_t target_c);

// Remove a custom profile by name (built-ins cannot be removed). Persists.
esp_err_t dc_bambu_zone_remove(const char *name);
