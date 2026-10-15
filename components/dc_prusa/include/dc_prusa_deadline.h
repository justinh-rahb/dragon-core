#pragma once

// Return a millisecond timeout that cannot extend past a monotonic deadline.
// Round down because the HTTP client accepts whole milliseconds; a sub-ms
// remainder is treated as expired rather than extending the deadline.
#include <stdint.h>

static inline int dc_prusa_deadline_timeout_ms(int64_t deadline_us, int64_t now_us)
{
    int64_t remaining_us = deadline_us - now_us;
    if (remaining_us < 1000) return 0;
    return (int)(remaining_us / 1000);
}
