#pragma once
// Pure, host-testable PrusaLink status freshness boundary (no ESP dependencies).
#include <stdbool.h>
#include <stdint.h>

// The normal poll period is 5 s. Three missed complete snapshots expire the source.
// sample_us <= 0 means no complete status has ever been received. A backwards
// monotonic-clock delta is treated as age zero rather than spuriously stale.
#define DC_PRUSA_STATUS_STALE_US 15000000LL

static inline bool dc_prusa_status_sample_fresh(int64_t now_us, int64_t sample_us)
{
    if (sample_us <= 0) return false;
    int64_t age_us = now_us - sample_us;
    if (age_us < 0) age_us = 0;
    return age_us <= DC_PRUSA_STATUS_STALE_US;
}
