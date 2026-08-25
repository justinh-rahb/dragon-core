// Host unit test for dc_prusa status freshness. Pure logic, no ESP deps.
#include "dc_prusa_freshness.h"
#include <stdio.h>

static int fails = 0;

static void expect_fresh(const char *name, int64_t now_us, int64_t sample_us, int want)
{
    int got = dc_prusa_status_sample_fresh(now_us, sample_us);
    int ok = got == want;
    if (!ok) fails++;
    printf("[%s] %-18s want=%d got=%d\n", ok ? "PASS" : "FAIL", name, want, got);
}

int main(void)
{
    expect_fresh("never received", 1000000LL, 0, 0);
    expect_fresh("fresh sample", 1000000LL, 999999LL, 1);
    expect_fresh("14.999999 s", 16000000LL, 1000001LL, 1);
    expect_fresh("exactly 15 s", 16000000LL, 1000000LL, 1);
    expect_fresh("15 s + 1 us", 16000001LL, 1000000LL, 0);
    expect_fresh("clock went back", 999999LL, 1000000LL, 1);

    printf(fails ? "\n%d FAILED\n" : "\nALL PASS\n", fails);
    return fails ? 1 : 0;
}
