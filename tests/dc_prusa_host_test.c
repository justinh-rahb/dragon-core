// Host unit test for dc_prusa status freshness/fail-cold filtering and the
// per-tool filament-slot pick used for /api/v1/job material detection.
#include "dc_prusa_freshness.h"
#include "dc_prusa_material.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

static int fails = 0;

static void expect_pick(const char *name, int count, const float *used,
                        bool used_known, int want)
{
    int got = dc_prusa_pick_material_slot(count, used, used_known);
    int ok = got == want;
    if (!ok) fails++;
    printf("[%s] pick %-22s want=%d got=%d\n", ok ? "PASS" : "FAIL",
           name, want, got);
}

// Material rides the status sample: retained while fresh, cleared when stale.
static void expect_material(const char *name, int64_t now_us, int64_t sample_us,
                            const char *want)
{
    dc_prusa_status_t status = { .state = DC_PRUSA_ONLINE, .online = true,
                                 .bed_temp = 56.0f, .bed_target = 70.0f };
    strcpy(status.printer_state, "PRINTING");
    strcpy(status.material, "PETG");
    dc_prusa_status_apply_freshness(&status, now_us, sample_us);
    int ok = strcmp(status.material, want) == 0;
    if (!ok) fails++;
    printf("[%s] material %-16s want=\"%s\" got=\"%s\"\n", ok ? "PASS" : "FAIL",
           name, want, status.material);
}

static void expect_fresh(const char *name, int64_t now_us, int64_t sample_us, int want)
{
    int got = dc_prusa_status_sample_fresh(now_us, sample_us);
    int ok = got == want;
    if (!ok) fails++;
    printf("[%s] fresh %-16s want=%d got=%d\n", ok ? "PASS" : "FAIL",
           name, want, got);
}

static void expect_snapshot(const char *name,
                            dc_prusa_status_t status,
                            int64_t now_us,
                            int64_t sample_us,
                            dc_prusa_state_t want_state,
                            int want_online,
                            int want_values,
                            uint32_t want_age_ms)
{
    dc_prusa_status_apply_freshness(&status, now_us, sample_us);
    int values_ok = want_values
        ? isfinite(status.bed_temp) && status.bed_target == 70.0f &&
          strcmp(status.printer_state, "PRINTING") == 0
        : isnan(status.bed_temp) && status.bed_target == 0.0f &&
          status.printer_state[0] == '\0';
    int ok = status.state == want_state &&
             status.online == (bool)want_online &&
             status.status_age_ms == want_age_ms &&
             values_ok;
    if (!ok) fails++;
    printf("[%s] snapshot %-13s state=%d online=%d age=%u values=%s\n",
           ok ? "PASS" : "FAIL", name, status.state, status.online,
           status.status_age_ms, values_ok ? "ok" : "wrong");
}

static dc_prusa_status_t populated(dc_prusa_state_t state, bool online)
{
    dc_prusa_status_t status = {
        .state = state,
        .online = online,
        .bed_temp = 56.0f,
        .bed_target = 70.0f,
    };
    strcpy(status.printer_state, "PRINTING");
    return status;
}

int main(void)
{
    // Timeout boundary: exactly 15 s remains fresh; the first microsecond after
    // it is stale. A backwards monotonic clock is clamped to age zero.
    expect_fresh("never received", 1000000LL, 0, 0);
    expect_fresh("fresh sample", 1000000LL, 999999LL, 1);
    expect_fresh("14.999999 s", 16000000LL, 1000001LL, 1);
    expect_fresh("exactly 15 s", 16000000LL, 1000000LL, 1);
    expect_fresh("15 s + 1 us", 16000001LL, 1000000LL, 0);
    expect_fresh("clock went back", 999999LL, 1000000LL, 1);

    // Full snapshot contract used by dc_prusa_get_status().
    expect_snapshot("fresh online", populated(DC_PRUSA_ONLINE, true),
                    2000000LL, 1000000LL,
                    DC_PRUSA_ONLINE, 1, 1, 1000);
    expect_snapshot("stale online", populated(DC_PRUSA_ONLINE, true),
                    16001000LL, 1000000LL,
                    DC_PRUSA_OFFLINE, 0, 0, 15001);
    expect_snapshot("stale connect", populated(DC_PRUSA_CONNECTING, false),
                    16001000LL, 1000000LL,
                    DC_PRUSA_OFFLINE, 0, 0, 15001);
    expect_snapshot("auth retained", populated(DC_PRUSA_AUTH_FAILED, false),
                    16001000LL, 1000000LL,
                    DC_PRUSA_AUTH_FAILED, 0, 0, 15001);
    expect_snapshot("never sampled", populated(DC_PRUSA_DISABLED, false),
                    1000000LL, 0,
                    DC_PRUSA_DISABLED, 0, 0, UINT32_MAX);
    expect_snapshot("clock rollback", populated(DC_PRUSA_ONLINE, true),
                    999999LL, 1000000LL,
                    DC_PRUSA_ONLINE, 1, 1, 0);

    // Material rides the status sample.
    expect_material("fresh keeps",  2000000LL, 1000000LL, "PETG");
    expect_material("stale clears", 16001000LL, 1000000LL, "");

    // Per-tool filament pick for /api/v1/job "filament_type per tool".
    expect_pick("no slots",         0,  NULL, false, -1);
    expect_pick("single no usage",  1,  NULL, false,  0);
    float u_slot1[] = {0.0f, 939.9f, 0.0f, 0.0f};   // XL: only tool 1 printed
    expect_pick("multi tool1 used", 4,  u_slot1, true, 1);
    float u_slot0[] = {512.0f, 0.0f};                // primary tool printed
    expect_pick("multi tool0 used", 2,  u_slot0, true, 0);
    float u_slot3[] = {0.0f, 0.0f, 0.0f, 88.1f};     // last of four
    expect_pick("multi tool3 used", 4,  u_slot3, true, 3);
    float u_none[]  = {0.0f, 0.0f};                   // usage present but all zero
    expect_pick("usage all zero",   2,  u_none, true, 0);
    float u_multi[] = {10.0f, 200.0f};               // most-used wins
    expect_pick("most used wins",   2,  u_multi, true, 1);
    expect_pick("array no usage",   3,  NULL, false,  0);   // usage unknown -> slot 0

    printf(fails ? "\n%d FAILED\n" : "\nALL PASS\n", fails);
    return fails ? 1 : 0;
}
