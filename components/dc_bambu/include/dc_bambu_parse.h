#pragma once
// Pure, host-testable Bambu report parsing (no ESP deps) — mirrors the dc_ntc /
// dc_heater convention of keeping decision logic inline in a header so it can be
// unit-tested on the host. Used by dc_bambu.c and tests/dc_bambu_host_test.c.
#include <string.h>
#include <strings.h>   // strncasecmp
#include <stdlib.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <errno.h>

// Result of resolving the active filament from a report. The tri-state matters:
// a delta report that OMITS filament state must NOT clobber the last known value,
// whereas an explicit "nothing loaded" MUST clear it (else a stale zone applies).
typedef enum {
    DC_FILA_ABSENT = 0,   // no filament state in this (delta) report -> keep prior
    DC_FILA_EMPTY,        // report explicitly has no active/loaded filament -> clear
    DC_FILA_PRESENT,      // an active filament type was found -> use `out`
} dc_fila_result_t;

/* Normalized form of the raw Bambu `gcode_state` token.  Keep it independent
 * of ESP-IDF/public API types so this parser stays host-testable. */
typedef enum {
    DC_BAMBU_GCODE_UNKNOWN,
    DC_BAMBU_GCODE_IDLE,
    DC_BAMBU_GCODE_DOWNLOADING,
    DC_BAMBU_GCODE_PREPARING,
    DC_BAMBU_GCODE_PRINTING,
    DC_BAMBU_GCODE_PAUSED,
    DC_BAMBU_GCODE_COMPLETE,
    DC_BAMBU_GCODE_ERROR,
} dc_bambu_gcode_phase_t;

static inline dc_bambu_gcode_phase_t dc_bambu_gcode_phase(const char *state)
{
    if (!state || !state[0]) return DC_BAMBU_GCODE_UNKNOWN;
    if (strcmp(state, "IDLE") == 0) return DC_BAMBU_GCODE_IDLE;
    /* SLICING is Bambu's pre-print transfer/download phase.  The latter two
     * spellings make the normalizer tolerant of firmware-family variants. */
    if (strcmp(state, "SLICING") == 0 || strcmp(state, "DOWNLOAD") == 0 ||
        strcmp(state, "DOWNLOADING") == 0) return DC_BAMBU_GCODE_DOWNLOADING;
    if (strcmp(state, "PREPARE") == 0) return DC_BAMBU_GCODE_PREPARING;
    if (strcmp(state, "RUNNING") == 0) return DC_BAMBU_GCODE_PRINTING;
    if (strcmp(state, "PAUSE") == 0) return DC_BAMBU_GCODE_PAUSED;
    if (strcmp(state, "FINISH") == 0) return DC_BAMBU_GCODE_COMPLETE;
    if (strcmp(state, "FAILED") == 0) return DC_BAMBU_GCODE_ERROR;
    return DC_BAMBU_GCODE_UNKNOWN;
}

static inline const char *dc_bambu_json_string_end(const char *q)
{
    if (!q || *q != '"') return NULL;
    for (q++; *q; q++) {
        if (*q == '\\' && q[1]) { q++; continue; }
        if (*q == '"') return q;
    }
    return NULL;
}

static inline const char *dc_bambu_json_object_end(const char *object)
{
    if (!object || *object != '{') return NULL;
    int depth = 0;
    for (const char *p = object; *p; p++) {
        if (*p == '"') {
            p = dc_bambu_json_string_end(p);
            if (!p) return NULL;
        } else if (*p == '{' || *p == '[') {
            depth++;
        } else if (*p == '}' || *p == ']') {
            if (--depth == 0) return p;
            if (depth < 0) return NULL;
        }
    }
    return NULL;
}

static inline const char *dc_bambu_json_array_end(const char *array)
{
    if (!array || *array != '[') return NULL;
    int depth = 0;
    for (const char *p = array; *p; p++) {
        if (*p == '"') {
            p = dc_bambu_json_string_end(p);
            if (!p) return NULL;
        } else if (*p == '{' || *p == '[') {
            depth++;
        } else if (*p == '}' || *p == ']') {
            if (--depth == 0) return p;
            if (depth < 0) return NULL;
        }
    }
    return NULL;
}

static inline const char *dc_bambu_json_direct_member(const char *object,
                                                       const char *key)
{
    const char *limit = dc_bambu_json_object_end(object);
    if (!limit) return NULL;
    size_t key_len = strlen(key);
    int depth = 0;
    for (const char *p = object + 1; p < limit; p++) {
        if (*p == '"') {
            const char *end = dc_bambu_json_string_end(p);
            if (!end || end > limit) return NULL;
            if (depth == 0 && (size_t)(end - p - 1) == key_len &&
                memcmp(p + 1, key, key_len) == 0) {
                const char *value = end + 1;
                while (value < limit && (*value == ' ' || *value == '\t' ||
                       *value == '\n' || *value == '\r')) value++;
                if (value >= limit || *value != ':') return NULL;
                value++;
                while (value < limit && (*value == ' ' || *value == '\t' ||
                       *value == '\n' || *value == '\r')) value++;
                return value < limit ? value : NULL;
            }
            p = end;
        } else if (*p == '{' || *p == '[') {
            depth++;
        } else if (*p == '}' || *p == ']') {
            if (depth == 0) return NULL;
            depth--;
        }
    }
    return NULL;
}

// Bambu's print_error can be a JSON number or a quoted decimal/hex token. Only
// accept the direct print.print_error member; nested/historical fields are not
// the live printer error.
static inline bool dc_bambu_print_error_code(const char *json, uint32_t *out)
{
    const char *q = dc_bambu_json_direct_member(json, "print");
    if (!q || *q != '{') return false;
    q = dc_bambu_json_direct_member(q, "print_error");
    if (!q) return false;
    bool quoted = *q == '"';
    if (quoted) q++;
    char *end = NULL;
    errno = 0;
    unsigned long long value = strtoull(q, &end, 0);
    if (end == q || errno == ERANGE || value > UINT32_MAX) return false;
    if (quoted) {
        if (*end != '"') return false;
    } else if (*end && *end != ',' && *end != '}' && *end != ' ' &&
               *end != '\t' && *end != '\n' && *end != '\r') {
        return false;
    }
    if (out) *out = (uint32_t)value;
    return true;
}

static inline uint32_t dc_bambu_next_print_error_code(
    uint32_t current, bool got_phase, dc_bambu_gcode_phase_t phase,
    bool got_error_code, uint32_t incoming)
{
    if (got_error_code) return incoming;
    if (got_phase && phase != DC_BAMBU_GCODE_ERROR) return 0;
    return current;
}

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

// Find a named object without requiring the enclosing Bambu push-status message
// to be complete. X1 reports can exceed the controller's reassembly buffer, but
// the AMS object is near the front and is normally received in full.
static inline const char *dc_bambu_json_find_object(const char *json, const char *quoted_key)
{
    const char *key = strstr(json, quoted_key);
    if (!key) return NULL;
    const char *value = strchr(key + strlen(quoted_key), ':');
    if (!value) return NULL;
    do { value++; } while (*value == ' ' || *value == '\t' ||
                           *value == '\n' || *value == '\r');
    return *value == '{' ? value : NULL;
}

static inline bool dc_bambu_json_member_string(const char *object, const char *key,
                                                char *out, size_t outsz)
{
    const char *value = dc_bambu_json_direct_member(object, key);
    if (!value || *value != '"' || outsz == 0) return false;
    value++;
    size_t i = 0;
    while (*value && *value != '"' && i + 1 < outsz) out[i++] = *value++;
    out[i] = '\0';
    return *value == '"';
}

static inline bool dc_bambu_json_member_long(const char *object, const char *key,
                                              long *out)
{
    const char *value = dc_bambu_json_direct_member(object, key);
    if (!value) return false;
    bool quoted = *value == '"';
    if (quoted) value++;
    char *end = NULL;
    long parsed = strtol(value, &end, 10);
    if (end == value || (quoted && *end != '"')) return false;
    if (out) *out = parsed;
    return true;
}

static inline const char *dc_bambu_json_next_array_object(const char *cursor,
                                                           const char *array)
{
    const char *limit = dc_bambu_json_array_end(array);
    if (!limit) return NULL;
    for (const char *p = cursor ? cursor : array + 1; p < limit; p++) {
        if (*p == '"') {
            p = dc_bambu_json_string_end(p);
            if (!p) return NULL;
        } else if (*p == '{') {
            return p;
        }
    }
    return NULL;
}

// Resolve the ACTIVE filament type from a Bambu report. Bambu lists loaded filaments
// under print.ams (AMS units, tray[].tray_type) and print.vt_tray (external spool),
// with print.ams.tray_now selecting the active global tray index (254 = external
// spool, 255 = none loaded). Targeted scan — no full JSON parse over ~15 KB.
static inline dc_fila_result_t dc_bambu_active_filament(const char *json, char *out, size_t outsz)
{
    out[0] = '\0';
    const char *ams = dc_bambu_json_find_object(json, "\"ams\"");
    const char *vt = dc_bambu_json_find_object(json, "\"vt_tray\"");
    if ((!ams || *ams != '{') && (!vt || *vt != '{')) return DC_FILA_ABSENT;

    long now = -1;
    bool got_now = ams && *ams == '{' &&
                   dc_bambu_json_member_long(ams, "tray_now", &now);
    char tt[16] = {0};

    if (now == 255) return DC_FILA_EMPTY;   // tray_now explicitly says nothing is loaded

    // AMS slot selected: resolve the active tray's type ONLY IF this report carries it.
    // A delta may name the slot (tray_now) without including the tray payload — that is
    // ABSENT (keep the last known filament), not empty. Only an active tray whose
    // tray_type is explicitly present-and-empty counts as EMPTY.
    if (got_now && now >= 0 && now < 250) {
        // Four-slot AMS families flatten unit/slot as unit * 4 + slot. AMS HT
        // units instead use hardware IDs 128..152 and contain one slot (0), so
        // tray_now is the unit ID itself.
        const bool is_ams_ht = now >= 128 && now < 153;
        const long wanted_ams = is_ams_ht ? now : now / 4;
        const long wanted_tray = is_ams_ht ? 0 : now % 4;
        const char *units = dc_bambu_json_direct_member(ams, "ams");
        if (units && *units == '[') {
            const char *unit = NULL;
            while ((unit = dc_bambu_json_next_array_object(
                        unit ? dc_bambu_json_object_end(unit) + 1 : NULL, units))) {
                long unit_id = -1;
                if (!dc_bambu_json_member_long(unit, "id", &unit_id) ||
                    unit_id != wanted_ams) continue;
                const char *trays = dc_bambu_json_direct_member(unit, "tray");
                if (!trays || *trays != '[') break;
                const char *tray = NULL;
                while ((tray = dc_bambu_json_next_array_object(
                            tray ? dc_bambu_json_object_end(tray) + 1 : NULL, trays))) {
                    long tray_id = -1;
                    if (!dc_bambu_json_member_long(tray, "id", &tray_id) ||
                        tray_id != wanted_tray) continue;
                    if (!dc_bambu_json_member_string(tray, "tray_type", tt, sizeof tt))
                        return DC_FILA_EMPTY;
                    if (tt[0]) { snprintf(out, outsz, "%s", tt); return DC_FILA_PRESENT; }
                    return DC_FILA_EMPTY;
                }
                break;
            }
        }
        return DC_FILA_ABSENT;   // selected tray's payload not in this (delta) report
    }

    // External spool (tray_now 254, or tray_now absent with a vt_tray present).
    if (vt && *vt == '{' && dc_bambu_json_member_string(vt, "tray_type", tt, sizeof tt)) {
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


// Chamber-temperature sample freshness. Keep this decision pure/host-testable so
// dc_bambu_get_status() and its tests share the exact timeout boundary.
// sample_us <= 0 means no chamber sample has ever been received. A backwards
// clock delta is treated as age zero rather than spuriously stale.
#define DC_BAMBU_CHAMBER_STALE_US 15000000LL

static inline bool dc_bambu_chamber_sample_fresh(int64_t now_us, int64_t sample_us)
{
    if (sample_us <= 0) return false;
    int64_t age_us = now_us - sample_us;
    if (age_us < 0) age_us = 0;
    return age_us <= DC_BAMBU_CHAMBER_STALE_US;
}

// Is a Bambu gcode_state "active" for chamber-zone purposes? PREPARE (so the chamber
// preheats before the print), RUNNING, and PAUSE (hold heat across a pause) are active;
// IDLE / FINISH / FAILED / SLICING are not. Case-sensitive: Bambu emits upper-case.
static inline bool dc_bambu_gcode_active(const char *state)
{
    if (!state || !state[0]) return false;
    dc_bambu_gcode_phase_t phase = dc_bambu_gcode_phase(state);
    return phase == DC_BAMBU_GCODE_PRINTING
        || phase == DC_BAMBU_GCODE_PREPARING
        || phase == DC_BAMBU_GCODE_PAUSED;
}
