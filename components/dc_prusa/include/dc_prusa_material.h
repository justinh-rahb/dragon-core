#pragma once

// Pure filament-slot selection for the PrusaLink /api/v1/job material, factored
// out of dc_prusa so it can be host-tested in isolation.
//
// PrusaLink's job metadata carries the printing filament as either a single
// scalar `filament_type` (single-toolhead: Core One, MK4) or, on a multi-toolhead
// printer (XL), a per-tool list `"filament_type per tool"` with a matching
// `"filament used [mm] per tool"` usage array. Unlike Klipper's active-tool index,
// the PrusaLink job endpoint gives no reliable "active tool" for a single print,
// so the printed slot is simply the one with non-zero usage (the filament actually
// being laid down). This mirrors the dc_moonraker #65 "follow the filament
// actually printed" fix, adapted to Prusa's usage-only signal.

#include <stdbool.h>

// Return the per-tool filament slot to follow, or -1 when there are no slots.
// - count:      number of parsed "filament_type per tool" entries
// - used_mm:    matching per-slot "filament used [mm] per tool" (may be NULL)
// - used_known: whether `used_mm` carries real per-slot usage
//
// With usage known, pick the most-used slot (the printed one). Without usage,
// fall back to slot 0 (the primary tool).
static inline int dc_prusa_pick_material_slot(int count, const float *used_mm,
                                              bool used_known)
{
    if (count <= 0) return -1;
    if (!used_known || !used_mm) return 0;

    int best = 0;
    float best_used = used_mm[0];
    for (int i = 1; i < count; ++i) {
        if (used_mm[i] > best_used) {
            best_used = used_mm[i];
            best = i;
        }
    }
    return best;
}
