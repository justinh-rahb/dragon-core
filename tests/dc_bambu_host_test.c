// Host unit test for the Bambu report parser (dc_bambu_parse.h): active-filament
// tri-state resolution and filament->zone matching. Pure logic, no ESP deps.
#include "dc_bambu_parse.h"
#include <stdio.h>
#include <string.h>

static int fails = 0;

static void expect_fila(const char *name, const char *json,
                        dc_fila_result_t want_r, const char *want_s)
{
    char got[16];
    dc_fila_result_t r = dc_bambu_active_filament(json, got, sizeof got);
    int ok = (r == want_r) && (want_r != DC_FILA_PRESENT || strcmp(got, want_s) == 0);
    if (!ok) fails++;
    static const char *R[] = { "ABSENT", "EMPTY", "PRESENT" };
    printf("[%s] %-22s want=%s/%-6s got=%s/%s\n", ok ? "PASS" : "FAIL", name,
           R[want_r], want_r == DC_FILA_PRESENT ? want_s : "-",
           R[r], r == DC_FILA_PRESENT ? got : "-");
}

static void expect_zone(const char *filament, int want_idx)
{
    static const char *const names[] = { "PLA", "PETG", "ABS", "ASA", "PC", "TPU" };
    int got = dc_bambu_zone_match(filament, names, 6);
    int ok = got == want_idx;
    if (!ok) fails++;
    printf("[%s] zone %-12s want=%d got=%d\n", ok ? "PASS" : "FAIL", filament, want_idx, got);
}

static void expect_active(const char *state, int want)
{
    int got = dc_bambu_gcode_active(state);
    int ok = got == want;
    if (!ok) fails++;
    printf("[%s] gcode %-9s want=%d got=%d\n", ok ? "PASS" : "FAIL", state, want, got);
}

static void expect_phase(const char *state, dc_bambu_gcode_phase_t want)
{
    dc_bambu_gcode_phase_t got = dc_bambu_gcode_phase(state);
    int ok = got == want;
    if (!ok) fails++;
    printf("[%s] phase %-9s want=%d got=%d\n", ok ? "PASS" : "FAIL", state, want, got);
}


static void expect_chamber_fresh(const char *name, int64_t now_us,
                                 int64_t sample_us, int want)
{
    int got = dc_bambu_chamber_sample_fresh(now_us, sample_us);
    int ok = got == want;
    if (!ok) fails++;
    printf("[%s] chamber-fresh %-18s want=%d got=%d\n",
           ok ? "PASS" : "FAIL", name, want, got);
}

int main(void)
{
    // --- active-filament tri-state ---
    // PRESENT: AMS active tray carries a type.
    expect_fila("ams tray0 PETG",
        "{\"print\":{\"ams\":{\"ams\":[{\"tray\":[{\"tray_type\":\"PETG\"},{\"tray_type\":\"PLA\"}]}],\"tray_now\":\"0\"},\"vt_tray\":{\"tray_type\":\"\"}}}",
        DC_FILA_PRESENT, "PETG");
    expect_fila("ams tray1 PLA",
        "{\"print\":{\"ams\":{\"ams\":[{\"tray\":[{\"tray_type\":\"PETG\"},{\"tray_type\":\"PLA\"}]}],\"tray_now\":\"1\"}}}",
        DC_FILA_PRESENT, "PLA");
    // PRESENT: external spool.
    expect_fila("ext spool ABS",
        "{\"print\":{\"ams\":{\"tray_now\":\"254\"},\"vt_tray\":{\"id\":\"254\",\"tray_type\":\"ABS\"}}}",
        DC_FILA_PRESENT, "ABS");
    // EMPTY: explicit no-spool (tray_now 255) MUST clear a prior value.
    expect_fila("none tray_now=255",
        "{\"print\":{\"ams\":{\"tray_now\":\"255\"},\"vt_tray\":{\"tray_type\":\"\"}}}",
        DC_FILA_EMPTY, NULL);
    // EMPTY: active AMS slot with an empty type (unloaded) -> clear.
    expect_fila("ams tray0 empty",
        "{\"print\":{\"ams\":{\"ams\":[{\"tray\":[{\"tray_type\":\"\"}]}],\"tray_now\":\"0\"}}}",
        DC_FILA_EMPTY, NULL);
    // ABSENT: a delta report that omits filament state -> keep prior (no clobber).
    expect_fila("delta no filament",
        "{\"print\":{\"bed_temper\":60.0,\"chamber_temper\":40.0}}",
        DC_FILA_ABSENT, NULL);
    // ABSENT: partial-AMS delta — slot is named (tray_now) but the tray payload is
    // absent. Must NOT clear a known filament (review round 2).
    expect_fila("partial-ams delta",
        "{\"print\":{\"ams\":{\"tray_now\":\"0\"}}}",
        DC_FILA_ABSENT, NULL);
    // ABSENT: external selected (254) but no vt_tray payload in this delta -> keep.
    expect_fila("ext selected, no vt",
        "{\"print\":{\"ams\":{\"tray_now\":\"254\"}}}",
        DC_FILA_ABSENT, NULL);
    // EMPTY: external spool present but explicitly empty -> clear.
    expect_fila("ext spool empty",
        "{\"print\":{\"vt_tray\":{\"id\":\"254\",\"tray_type\":\"\"}}}",
        DC_FILA_EMPTY, NULL);
    // The reviewer's PETG -> no-spool transition: an EMPTY report tells parse_report
    // to clear the stored PETG (see dc_bambu.c); ABSENT deltas leave it untouched.

    // --- filament -> zone matching (case-insensitive prefix) ---
    expect_zone("PETG", 1);
    expect_zone("PETG-CF", 1);
    expect_zone("PLA Basic", 0);
    expect_zone("ASA-CF", 3);
    expect_zone("petg", 1);      // case-insensitive
    expect_zone("PVA", -1);      // unknown -> no zone
    expect_zone("", -1);

    // --- longest-prefix wins when a custom profile shadows a built-in ---
    // Combined built-ins + customs, exactly as dc_bambu_zone_target builds the array.
    {
        static const char *const combo[] = { "PLA","PETG","ABS","ASA","PC","TPU","PETG-CF","PA" };
        int n = 8;
        struct { const char *fil; int want; } cases[] = {
            { "PETG-CF Bambu", 6 },   // custom "PETG-CF" beats built-in "PETG"
            { "PETG",          1 },   // plain PETG still the built-in
            { "PAHT-CF",       7 },   // custom "PA" prefix matches
            { "PA6-GF",        7 },
            { "PC",            4 },
            { "PVA",          -1 },
        };
        for (unsigned i = 0; i < sizeof cases / sizeof cases[0]; i++) {
            int got = dc_bambu_zone_match(cases[i].fil, combo, n);
            int ok = got == cases[i].want;
            if (!ok) fails++;
            printf("[%s] custom-zone %-14s want=%d got=%d\n", ok ? "PASS" : "FAIL",
                   cases[i].fil, cases[i].want, got);
        }
    }


    // --- chamber-temperature freshness ---
    // Never-received samples are unavailable; the timeout boundary itself is
    // still fresh, and the first microsecond beyond it is stale. A backwards
    // monotonic-clock delta is clamped to age zero by the helper.
    expect_chamber_fresh("never received", 1000000LL, 0, 0);
    expect_chamber_fresh("fresh sample", 1000000LL, 999999LL, 1);
    expect_chamber_fresh("14.999999 s", 16000000LL, 1000001LL, 1);
    expect_chamber_fresh("exactly 15 s", 16000000LL, 1000000LL, 1);
    expect_chamber_fresh("15 s + 1 us", 16000001LL, 1000000LL, 0);
    expect_chamber_fresh("clock went back", 999999LL, 1000000LL, 1);

    // --- print-state classifier (preheat starts on PREPARE) ---
    expect_active("PREPARE", 1);
    expect_active("RUNNING", 1);
    expect_active("PAUSE",   1);
    expect_active("IDLE",    0);
    expect_active("FINISH",  0);
    expect_active("FAILED",  0);
    expect_active("",        0);

    // --- normalized gcode phases, including the H2D download indication ---
    expect_phase("SLICING",     DC_BAMBU_GCODE_DOWNLOADING);
    expect_phase("DOWNLOAD",    DC_BAMBU_GCODE_DOWNLOADING);
    expect_phase("PREPARE",     DC_BAMBU_GCODE_PREPARING);
    expect_phase("RUNNING",     DC_BAMBU_GCODE_PRINTING);
    expect_phase("PAUSE",       DC_BAMBU_GCODE_PAUSED);
    expect_phase("FINISH",      DC_BAMBU_GCODE_COMPLETE);
    expect_phase("FAILED",      DC_BAMBU_GCODE_ERROR);
    expect_phase("IDLE",        DC_BAMBU_GCODE_IDLE);
    expect_phase("UNRECOGNIZED", DC_BAMBU_GCODE_UNKNOWN);

    printf(fails ? "\n%d FAILED\n" : "\nALL PASS\n", fails);
    return fails ? 1 : 0;
}
