// Host unit test for dc_prusa status freshness and fail-cold filtering.
#include "dc_prusa_freshness.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

static int fails = 0;

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

    printf(fails ? "\n%d FAILED\n" : "\nALL PASS\n", fails);
    return fails ? 1 : 0;
}
