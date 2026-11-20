#pragma once

// Small in-memory ring buffer of human-readable event lines so the portal can
// show what's been happening without the user needing serial access. Nothing
// clever: fixed slot count, oldest overwritten first, snapshot copied under a
// lock. Add is safe from any task including ISRs-that-can-take-a-mutex (which
// means: not real ISRs — this is a coarse-grained mutex, not a spinlock).

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

#define DC_EVLOG_MAX_ENTRIES  64
#define DC_EVLOG_TEXT_BYTES   96

typedef struct {
    uint32_t ms;                    // millis since boot
    char     text[DC_EVLOG_TEXT_BYTES];
} dc_evlog_entry_t;

// Initialise the log. Safe to call before any producer. If already initialised,
// no-op.
void dc_evlog_init(void);

// Append a printf-formatted line. Truncated to DC_EVLOG_TEXT_BYTES-1. Safe to
// call from any task once dc_evlog_init has been called; also safe to call
// before init (silently drops the line — no crash).
void dc_evlog_add(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

// Copy up to `max` most-recent entries into `out`, newest first. Returns how
// many were copied.
size_t dc_evlog_snapshot(dc_evlog_entry_t *out, size_t max);

// ---- firmware console capture (raw ESP_LOGx stream) ----
// Separate from the curated event ring above: a fixed byte ring that captures the
// full ESP_LOGx output via an esp_log_set_vprintf hook while still forwarding to
// UART. Motivation: newer Panda hardware has no external USB, and release builds
// repurpose the UART TX pin (GPIO21) as the Power LED — so the serial console is
// otherwise unreachable. Served by the /console page.
#define DC_EVLOG_CONSOLE_BYTES  16384   // ~180 lines; ~3% of worst-case free heap

// Install the log-capture hook. Call ONCE, as early as possible in app_main, so
// the boot log is captured. Idempotent. Uses a spinlock (safe from any context);
// never logs from within the hook (no recursion).
void dc_evlog_console_init(void);

// Compatibility API for callers that need one atomic oldest -> newest copy of
// the console ring. `out` is always NUL-terminated and the return value excludes
// that NUL. The caller owns the storage; callers requesting the complete ring
// should avoid placing such a large buffer on a constrained task stack. Streaming
// HTTP paths should prefer the bounded snapshot-view API below.
size_t dc_evlog_console_snapshot(char *out, size_t max);

// Bounded-memory console snapshot view for streaming callers. `begin` captures a
// logical oldest->newest view and copies its first chunk atomically with that
// capture, so a producer cannot invalidate byte zero between those operations.
// `read` copies later chunks without holding the console spinlock across caller
// I/O. If logging has advanced far enough to overwrite unread bytes from the
// captured view, `read` returns false instead of mixing two snapshots.
typedef struct {
    size_t start;
    size_t len;
    uint64_t write_seq;
} dc_evlog_console_view_t;

size_t dc_evlog_console_snapshot_begin(dc_evlog_console_view_t *view,
                                       char *out, size_t max);
bool dc_evlog_console_snapshot_read(const dc_evlog_console_view_t *view,
                                    size_t offset, char *out, size_t max,
                                    size_t *written);

// ---- absolute-cursor console read ----
// Every byte ever appended to the console ring has an absolute sequence number
// for the current boot: the first captured byte is 0 and `write_seq` is the
// sequence number the next byte will receive. Sequence numbers restart at 0 on
// every boot, so a cursor is only meaningful together with a product-owned boot
// identity (see dc_logtail).
//
// With W = write_seq and C = DC_EVLOG_CONSOLE_BYTES, the retained window is
// [oldest_seq, W) where oldest_seq = max(0, W - C). For a requested cursor:
//
//   oldest_seq <= cursor <= W  -> read starts exactly at cursor, lost_bytes 0
//   cursor < oldest_seq        -> bytes were overwritten; read starts at
//                                 oldest_seq, lost_bytes = oldest_seq - cursor
//   cursor > W                 -> DC_EVLOG_CONSOLE_READ_FUTURE_CURSOR; nothing
//                                 is copied and the cursor is never rewound
//
// All ranges are half-open. On success `out` holds exactly the ring bytes in
// [start_seq, end_seq), unmodified and not NUL-terminated, and the next cursor
// is end_seq. Loss is reported in raw bytes only; no line accounting.
typedef enum {
    DC_EVLOG_CONSOLE_READ_OK = 0,
    DC_EVLOG_CONSOLE_READ_FUTURE_CURSOR,
    DC_EVLOG_CONSOLE_READ_INVALID_ARG,
} dc_evlog_console_read_status_t;

typedef struct {
    uint64_t cursor;      // requested cursor, as passed in
    uint64_t oldest_seq;  // oldest retained byte at the time of the read
    uint64_t start_seq;   // first byte copied (== end_seq when none)
    uint64_t end_seq;     // one past the last byte copied; the next cursor
    uint64_t write_seq;   // producer position observed by this read
    uint64_t lost_bytes;  // start_seq - cursor
    size_t   len;         // bytes copied == end_seq - start_seq
    size_t   capacity;    // DC_EVLOG_CONSOLE_BYTES
} dc_evlog_console_read_t;

// Copies up to `max` bytes starting at `cursor` into `out` as one coherent
// interval taken under a single short console critical section (the lock is
// held only for the bounded copy). `out` may be NULL only when `max` is 0,
// which yields a metadata-only probe. `info` is required.
//
// OK:             every field of *info is valid.
// FUTURE_CURSOR:  cursor, oldest_seq, write_seq and capacity are valid;
//                 start_seq, end_seq, lost_bytes and len are 0.
// INVALID_ARG:    *info is zeroed when non-NULL.
//
// Callers should bound `max`: it bounds the critical-section copy length.
dc_evlog_console_read_status_t dc_evlog_console_read(uint64_t cursor,
                                                     char *out, size_t max,
                                                     dc_evlog_console_read_t *info);
