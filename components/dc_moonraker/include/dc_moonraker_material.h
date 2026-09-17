#pragma once

// Pure filament-slot selection for AUTO material detection, factored out of
// dc_moonraker so it can be host-tested in isolation.
//
// The slicer g-code metadata carries filament_type as a PER-SLICER-SLOT list
// ("PLA;PETG" -> slot 0 PLA, slot 1 PETG) and filament_used_mm as a matching
// per-slot usage array. The active tool (toolhead.extruder) is a PHYSICAL
// extruder index, which on a single-nozzle printer is ALWAYS 0 -- so a
// multi-filament project whose sliced object uses only a later slot would
// otherwise resolve to slot 0 and mis-drive AUTO. When usage is known and the
// active tool's slot was not printed (zero usage), fall back to the most-used
// slot, which is the filament actually being laid down.

#include <stdbool.h>

// Return the filament slot AUTO should follow, or -1 when there are no slots.
// - count:        number of parsed filament_type slots
// - active_tool:  toolhead.extruder index (clamped into range)
// - used:         per-slot filament_used_mm (may be NULL when unknown)
// - used_known:   whether `used` carries real per-slot usage
static inline int dc_moonraker_pick_material_slot(int count, int active_tool,
                                                  const float *used, bool used_known)
{
    if (count <= 0) return -1;
    int idx = active_tool;
    if (idx < 0) idx = 0;
    if (idx >= count) idx = count - 1;

    // Keep the active tool when its slot was actually printed (genuine
    // multi-material / a real tool change). Only when the active tool's slot has
    // zero usage do we switch to the most-used slot -- the single-nozzle
    // multi-filament case. With no usage data, the active-tool index stands.
    if (used_known && used && used[idx] <= 0.0f) {
        int best = idx;
        float best_used = 0.0f;
        for (int i = 0; i < count; ++i) {
            if (used[i] > best_used) {
                best_used = used[i];
                best = i;
            }
        }
        idx = best;
    }
    return idx;
}
