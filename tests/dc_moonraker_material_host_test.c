// Host unit test for dc_moonraker AUTO filament-slot selection.
#include "dc_moonraker_material.h"

#include <stdio.h>

static int fails = 0;

static void expect(const char *name, int count, int active,
                   const float *used, bool used_known, int want)
{
    int got = dc_moonraker_pick_material_slot(count, active, used, used_known);
    int ok = got == want;
    if (!ok) fails++;
    printf("[%s] %-42s want=%d got=%d\n", ok ? "PASS" : "FAIL", name, want, got);
}

int main(void)
{
    // No slots -> -1.
    expect("no slots", 0, 0, NULL, false, -1);

    // Single filament: trivially slot 0, with or without usage.
    float one_used[] = {123.0f};
    expect("single slot, no usage data", 1, 0, NULL, false, 0);
    expect("single slot, used", 1, 0, one_used, true, 0);

    // THE BUG: single-nozzle multi-filament project. Active tool is always 0, but
    // the sliced object printed only slot 1 (PETG). Must pick slot 1, not slot 0.
    float pla_petg[] = {0.0f, 12.5f};   // PLA unused, PETG printed
    expect("bug: active 0 unused -> most-used slot", 2, 0, pla_petg, true, 1);

    // Real U1 metadata: filament_type "PETG;PETG;PETG;PETG", used [13267,0,0,0].
    // Active tool 0 IS printed, so it stays on slot 0 (unchanged behaviour).
    float petg4[] = {13267.0f, 0.0f, 0.0f, 0.0f};
    expect("active 0 used -> keep slot 0", 4, 0, petg4, true, 0);

    // Genuine multi-material with a real tool change: active tool's slot is used,
    // so we keep the active tool rather than the most-used one.
    float multi[] = {50.0f, 5.0f};
    expect("active 1 used -> keep active tool", 2, 1, multi, true, 1);

    // No usage data (older Moonraker/slicer): fall back to the active tool index.
    expect("no usage -> active tool index", 2, 0, NULL, false, 0);
    expect("no usage -> active tool index (T1)", 2, 1, NULL, false, 1);

    // Active tool out of range is clamped into the list.
    expect("active tool clamped high", 2, 9, pla_petg, true, 1);
    expect("negative active tool clamped", 2, -3, petg4, true, 0);

    // Usage known but the active slot is used while another is larger: still keep
    // the active tool (only an UNUSED active slot triggers the fallback).
    float bigger_elsewhere[] = {10.0f, 100.0f};
    expect("active used, keep even if smaller", 2, 0, bigger_elsewhere, true, 0);

    // Usage known but ALL zero (degenerate): fallback loop finds nothing bigger,
    // so the clamped active tool stands.
    float all_zero[] = {0.0f, 0.0f};
    expect("all-zero usage -> active tool", 2, 1, all_zero, true, 1);

    printf(fails ? "\nMOONRAKER MATERIAL: %d FAIL\n" : "\ndc_moonraker material checks: PASS\n", fails);
    return fails ? 1 : 0;
}
