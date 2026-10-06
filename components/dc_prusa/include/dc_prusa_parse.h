#pragma once

// Pure PrusaLink JSON material parsers, factored out of dc_prusa so they can be
// host-tested against real Buddy/PrusaLink payloads (see tests). They take a JSON
// string and write a filament token to `out`; they touch no module state, no
// network, and no locks — the poll loop in dc_prusa.c owns all of that.

#include <stdbool.h>
#include <stddef.h>
#include <string.h>

#include "cJSON.h"
#include "dc_prusa_material.h"

#define DC_PRUSA_MAT_MAX    24   // filament_type token cap (matches status.material)
#define DC_PRUSA_TOOLS_MAX  5    // Prusa XL: up to 5 toolheads

// Parse a /api/v1/job body for the printing filament type into `out`. Reads
// file.meta."filament_type" (scalar, single-toolhead) or, when a per-tool list is
// present (Prusa XL), "filament_type per tool" indexed by the most-used slot from
// "filament used [mm] per tool". Returns true and writes a non-empty token on
// success; false leaves `out` untouched.
static inline bool dc_prusa_parse_job_material(const char *json, char *out, size_t out_sz)
{
    if (!json || !out || out_sz == 0) return false;
    cJSON *root = cJSON_Parse(json);
    if (!root) return false;

    bool ok = false;
    const char *picked = NULL;

    cJSON *file = cJSON_GetObjectItemCaseSensitive(root, "file");
    cJSON *meta = cJSON_IsObject(file)
        ? cJSON_GetObjectItemCaseSensitive(file, "meta") : NULL;
    if (cJSON_IsObject(meta)) {
        // Multi-toolhead (XL): "filament_type per tool" + usage array; pick the
        // printed slot (mirrors dc_moonraker #65). Falls through to the scalar.
        cJSON *types = cJSON_GetObjectItemCaseSensitive(meta, "filament_type per tool");
        if (cJSON_IsArray(types) && cJSON_GetArraySize(types) > 0) {
            int count = cJSON_GetArraySize(types);
            if (count > DC_PRUSA_TOOLS_MAX) count = DC_PRUSA_TOOLS_MAX;
            float used[DC_PRUSA_TOOLS_MAX];
            bool used_known = false;
            cJSON *usage = cJSON_GetObjectItemCaseSensitive(meta, "filament used [mm] per tool");
            if (cJSON_IsArray(usage) && cJSON_GetArraySize(usage) >= count) {
                used_known = true;
                for (int i = 0; i < count; ++i) {
                    cJSON *u = cJSON_GetArrayItem(usage, i);
                    used[i] = cJSON_IsNumber(u) ? (float)u->valuedouble : 0.0f;
                }
            }
            int idx = dc_prusa_pick_material_slot(count, used_known ? used : NULL,
                                                  used_known);
            if (idx >= 0) {
                cJSON *t = cJSON_GetArrayItem(types, idx);
                if (cJSON_IsString(t) && t->valuestring[0]) picked = t->valuestring;
            }
        }
        // Scalar filament_type (single-toolhead, or fallback).
        if (!picked) {
            cJSON *ft = cJSON_GetObjectItemCaseSensitive(meta, "filament_type");
            if (cJSON_IsString(ft) && ft->valuestring[0]) picked = ft->valuestring;
        }
    }

    if (picked) {
        strncpy(out, picked, out_sz - 1);
        out[out_sz - 1] = '\0';
        ok = true;
    }
    cJSON_Delete(root);
    return ok;
}

// Fallback material parse from the LEGACY OctoPrint-compatible endpoint
// `/api/printer`, which reports the loaded filament as `telemetry.material`. This
// is the only reliable material signal when /api/v1/job carries no file.meta —
// e.g. a file sent via PrusaLink onto USB, which PrusaLink does not parse. Writes
// a non-empty token to `out` and returns true on success. "---" means no filament
// loaded and is rejected.
static inline bool dc_prusa_parse_printer_material(const char *json, char *out, size_t out_sz)
{
    if (!json || !out || out_sz == 0) return false;
    cJSON *root = cJSON_Parse(json);
    if (!root) return false;

    bool ok = false;
    cJSON *tel = cJSON_GetObjectItemCaseSensitive(root, "telemetry");
    cJSON *mat = cJSON_IsObject(tel)
        ? cJSON_GetObjectItemCaseSensitive(tel, "material") : NULL;
    if (cJSON_IsString(mat) && mat->valuestring[0]
            && strcmp(mat->valuestring, "---") != 0) {   // "---" = no filament loaded
        strncpy(out, mat->valuestring, out_sz - 1);
        out[out_sz - 1] = '\0';
        ok = true;
    }
    cJSON_Delete(root);
    return ok;
}
