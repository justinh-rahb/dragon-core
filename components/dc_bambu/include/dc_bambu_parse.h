#pragma once
// Pure, host-testable Bambu report parsing (no ESP deps) — mirrors the dc_ntc /
// dc_heater convention of keeping decision logic inline in a header so it can be
// unit-tested on the host. Used by dc_bambu.c and tests/dc_bambu_host_test.c.
#include <string.h>
#include <strings.h>   // strncasecmp
#include <stdlib.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>

// Result of resolving the active filament from a report. The tri-state matters:
// a delta report that OMITS filament state must NOT clobber the last known value,
// whereas an explicit "nothing loaded" MUST clear it (else a stale zone applies).
typedef enum {
    DC_FILA_ABSENT = 0,   // no filament state in this (delta) report -> keep prior
    DC_FILA_EMPTY,        // report explicitly has no active/loaded filament -> clear
    DC_FILA_PRESENT,      // an active filament type was found -> use `out`
} dc_fila_result_t;

// Copy the string value of a JSON key ("key":"value") into out. `key` includes the
// quotes. Returns true if a (possibly empty) string value was found.
static inline bool dc_bambu_find_string(const char *s, const char *key, char *out, size_t outsz)
{
    const char *q = strstr(s, key);
    if (!q) return false;
    q = strchr(q, ':');
    if (!q) return false;
    q++;
    while (*q == ' ' || *q == '\t' || *q == '\n' || *q == '\r') q++;
    if (*q != '"') return false;
    q++;
    size_t i = 0;
    while (*q && *q != '"' && i + 1 < outsz) out[i++] = *q++;
    out[i] = '\0';
    return true;
}

// Resolve the ACTIVE filament type from a Bambu report. Bambu lists loaded filaments
// under print.ams (AMS units, tray[].tray_type) and print.vt_tray (external spool),
// with print.ams.tray_now selecting the active global tray index (254 = external
// spool, 255 = none loaded). Targeted scan — no full JSON parse over ~15 KB.
static inline dc_fila_result_t dc_bambu_active_filament(const char *json, char *out, size_t outsz)
{
    out[0] = '\0';
    const char *tn  = strstr(json, "\"tray_now\"");
    const char *vt  = strstr(json, "\"vt_tray\"");
    const char *ams = strstr(json, "\"ams\"");
    if (!tn && !vt && !ams) return DC_FILA_ABSENT;   // delta with no filament state

    char trayn[8] = {0};
    long now = (tn && dc_bambu_find_string(json, "\"tray_now\"", trayn, sizeof trayn))
                   ? strtol(trayn, NULL, 10) : -1;
    char tt[16];

    if (now == 255) return DC_FILA_EMPTY;   // tray_now explicitly says nothing is loaded

    // AMS slot selected: resolve the active tray's type ONLY IF this report carries it.
    // A delta may name the slot (tray_now) without including the tray payload — that is
    // ABSENT (keep the last known filament), not empty. Only an active tray whose
    // tray_type is explicitly present-and-empty counts as EMPTY.
    if (now >= 0 && now < 250) {
        if (ams) {
            const char *p = ams; long idx = -1;
            while ((p = strstr(p, "\"tray_type\"")) != NULL) {
                if (++idx == now) {                 // the active tray IS in this report
                    if (dc_bambu_find_string(p, "\"tray_type\"", tt, sizeof tt)) {
                        if (tt[0]) { snprintf(out, outsz, "%s", tt); return DC_FILA_PRESENT; }
                        return DC_FILA_EMPTY;        // active tray present but empty
                    }
                    break;
                }
                p += 11;
            }
        }
        return DC_FILA_ABSENT;   // selected tray's payload not in this (delta) report
    }

    // External spool (tray_now 254, or tray_now absent with a vt_tray present).
    if (vt && dc_bambu_find_string(vt, "\"tray_type\"", tt, sizeof tt)) {
        if (tt[0]) { snprintf(out, outsz, "%s", tt); return DC_FILA_PRESENT; }
        return DC_FILA_EMPTY;    // external spool present but empty
    }
    return DC_FILA_ABSENT;       // couldn't resolve -> don't clobber the last value
}

// Index of the zone whose base name is the LONGEST case-insensitive prefix of
// `filament` (so "PETG-CF" resolves to a custom "PETG-CF" over "PETG", and
// "PLA Basic" to "PLA"), or -1 if none match.
static inline int dc_bambu_zone_match(const char *filament, const char *const names[], int count)
{
    if (!filament || !filament[0]) return -1;
    int best = -1;
    size_t bestlen = 0;
    for (int i = 0; i < count; i++) {
        size_t n = strlen(names[i]);
        if (n > 0 && n > bestlen && strncasecmp(filament, names[i], n) == 0) {
            best = i;
            bestlen = n;
        }
    }
    return best;
}

// Is a Bambu gcode_state "active" for chamber-zone purposes? PREPARE (so the chamber
// preheats before the print), RUNNING, and PAUSE (hold heat across a pause) are active;
// IDLE / FINISH / FAILED / SLICING are not. Case-sensitive: Bambu emits upper-case.
static inline bool dc_bambu_gcode_active(const char *state)
{
    if (!state || !state[0]) return false;
    return strcmp(state, "RUNNING") == 0
        || strcmp(state, "PREPARE") == 0
        || strcmp(state, "PAUSE")   == 0;
}
