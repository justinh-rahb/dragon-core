#pragma once
// Pure, host-testable PrusaLink freshness and fail-cold snapshot filtering.
#include "dc_prusa.h"

#include <math.h>
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

// Derive the public snapshot from the last complete sample. This is deliberately
// pure: the cached poller state is untouched, while every caller gets the same
// fail-cold values if the poll task stops refreshing an apparently-online source.
static inline void dc_prusa_status_apply_freshness(dc_prusa_status_t *status,
                                                   int64_t now_us,
                                                   int64_t sample_us)
{
    if (!status) return;

    if (sample_us <= 0) {
        status->status_age_ms = UINT32_MAX;
    } else {
        int64_t age_us = now_us - sample_us;
        if (age_us < 0) age_us = 0;
        uint64_t age_ms = (uint64_t)age_us / 1000ULL;
        status->status_age_ms =
            age_ms > UINT32_MAX ? UINT32_MAX : (uint32_t)age_ms;
    }

    if (!dc_prusa_status_sample_fresh(now_us, sample_us)) {
        if (status->state == DC_PRUSA_ONLINE ||
            status->state == DC_PRUSA_CONNECTING)
            status->state = DC_PRUSA_OFFLINE;
        status->online = false;
        status->bed_temp = NAN;
        status->bed_target = 0.0f;
        status->printer_state[0] = '\0';
    }
}
