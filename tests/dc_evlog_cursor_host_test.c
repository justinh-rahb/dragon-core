// Host regression tests for dc_evlog_console_read(): absolute-cursor reads of
// the console byte ring. The ring is process-global and cannot be reset, so the
// tests run as one ordered scenario: empty ring, partial fill, one wrap, many
// wraps, then chained reads with producer progress between them.
//
// Every produced byte is a known function of its absolute sequence number and
// is pushed through the real esp_log vprintf hook one byte at a time with %c,
// so the stream includes NUL, CR/LF and 0x80-0xFF bytes.

#include "dc_evlog.h"

#include "esp_log.h"

#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define C ((uint64_t)DC_EVLOG_CONSOLE_BYTES)

static int s_failures;
static vprintf_like_t s_log_hook;
static uint64_t s_produced;   // model of write_seq

static int discard_vprintf(const char *fmt, va_list ap)
{
    (void)fmt;
    (void)ap;
    return 0;
}

vprintf_like_t esp_log_set_vprintf(vprintf_like_t func)
{
    vprintf_like_t previous = s_log_hook ? s_log_hook : discard_vprintf;
    s_log_hook = func;
    return previous;
}

static void capture_log(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    (void)s_log_hook(fmt, ap);
    va_end(ap);
}

static unsigned char byte_at(uint64_t seq)
{
    return (unsigned char)((seq * 131u + 7u) & 0xFFu);
}

static void produce(uint64_t n)
{
    for (uint64_t i = 0; i < n; ++i) {
        capture_log("%c", (int)byte_at(s_produced));
        s_produced++;
    }
}

static void produce_to(uint64_t target)
{
    if (target > s_produced) produce(target - s_produced);
}

static void expect_true(const char *name, bool condition)
{
    printf("[%s] %s\n", condition ? "PASS" : "FAIL", name);
    if (!condition) s_failures++;
}

static uint64_t model_oldest(void)
{
    return s_produced > C ? s_produced - C : 0;
}

static bool bytes_match(const char *buf, uint64_t start, size_t len)
{
    for (size_t i = 0; i < len; ++i)
        if ((unsigned char)buf[i] != byte_at(start + i)) return false;
    return true;
}

// Checks every contract invariant of a successful read against the model.
static bool read_ok(uint64_t cursor, char *buf, size_t max,
                    dc_evlog_console_read_t *info)
{
    dc_evlog_console_read_status_t st = dc_evlog_console_read(cursor, buf, max, info);
    uint64_t oldest = model_oldest();
    uint64_t start = cursor < oldest ? oldest : cursor;
    uint64_t avail = s_produced - start;
    uint64_t want = avail < max ? avail : max;
    return st == DC_EVLOG_CONSOLE_READ_OK &&
           info->cursor == cursor &&
           info->write_seq == s_produced &&
           info->oldest_seq == oldest &&
           info->start_seq == start &&
           info->lost_bytes == start - cursor &&
           info->end_seq == start + want &&
           info->end_seq - info->start_seq == info->len &&
           info->len == want &&
           info->capacity == DC_EVLOG_CONSOLE_BYTES &&
           info->end_seq <= info->write_seq &&
           info->oldest_seq <= info->start_seq &&
           (buf == NULL || bytes_match(buf, info->start_seq, info->len));
}

static bool future_rejected(uint64_t cursor)
{
    char buf[8];
    memset(buf, 0x5A, sizeof(buf));
    dc_evlog_console_read_t info;
    dc_evlog_console_read_status_t st =
        dc_evlog_console_read(cursor, buf, sizeof(buf), &info);
    bool untouched = true;
    for (size_t i = 0; i < sizeof(buf); ++i) untouched &= buf[i] == 0x5A;
    return st == DC_EVLOG_CONSOLE_READ_FUTURE_CURSOR &&
           info.cursor == cursor && info.write_seq == s_produced &&
           info.oldest_seq == model_oldest() &&
           info.capacity == DC_EVLOG_CONSOLE_BYTES &&
           info.start_seq == 0 && info.end_seq == 0 &&
           info.lost_bytes == 0 && info.len == 0 && untouched;
}

static void test_empty_ring(void)
{
    char buf[16];
    dc_evlog_console_read_t info;
    expect_true("empty ring: cursor 0 is a valid empty read",
                read_ok(0, buf, sizeof(buf), &info) && info.len == 0 &&
                info.start_seq == 0 && info.end_seq == 0 && info.write_seq == 0);
    expect_true("empty ring: cursor 1 is a future cursor", future_rejected(1));
}

static void test_argument_validation(void)
{
    char buf[4];
    dc_evlog_console_read_t info;
    memset(&info, 0xA5, sizeof(info));
    expect_true("NULL info is rejected",
                dc_evlog_console_read(0, buf, sizeof(buf), NULL) ==
                    DC_EVLOG_CONSOLE_READ_INVALID_ARG);
    expect_true("NULL out with nonzero max is rejected",
                dc_evlog_console_read(0, NULL, 4, &info) ==
                    DC_EVLOG_CONSOLE_READ_INVALID_ARG);
    dc_evlog_console_read_t zero = {0};
    expect_true("rejected read zeroes info", memcmp(&info, &zero, sizeof(info)) == 0);
    expect_true("NULL out with max 0 is a metadata probe",
                read_ok(0, NULL, 0, &info) && info.len == 0);
}

static void test_partial_ring(void)
{
    char buf[256];
    dc_evlog_console_read_t info;
    produce(100);

    expect_true("read from 0 returns all bytes",
                read_ok(0, buf, sizeof(buf), &info) && info.len == 100);
    expect_true("read from middle cursor starts exactly there",
                read_ok(40, buf, sizeof(buf), &info) &&
                info.start_seq == 40 && info.len == 60 && info.lost_bytes == 0);
    expect_true("response smaller than available data",
                read_ok(10, buf, 5, &info) && info.len == 5 &&
                info.start_seq == 10 && info.end_seq == 15);
    expect_true("cursor == write_seq is an empty read",
                read_ok(100, buf, sizeof(buf), &info) && info.len == 0 &&
                info.start_seq == 100 && info.end_seq == 100);
    expect_true("cursor == write_seq + 1 is rejected", future_rejected(101));
    expect_true("cursor UINT64_MAX is rejected", future_rejected(UINT64_MAX));
    expect_true("before wrap, oldest retained is 0",
                read_ok(0, buf, 1, &info) && info.oldest_seq == 0);
}

static void test_ring_exactly_full(void)
{
    char *buf = malloc(DC_EVLOG_CONSOLE_BYTES);
    dc_evlog_console_read_t info;
    produce_to(C);
    expect_true("exactly full ring: oldest is still 0, nothing lost",
                read_ok(0, buf, DC_EVLOG_CONSOLE_BYTES, &info) &&
                info.oldest_seq == 0 && info.lost_bytes == 0 && info.len == C);
    produce(1);
    expect_true("one byte past full: oldest 1, cursor 0 loses exactly 1",
                read_ok(0, buf, DC_EVLOG_CONSOLE_BYTES, &info) &&
                info.oldest_seq == 1 && info.lost_bytes == 1 && info.len == C);
    free(buf);
}

static void test_single_wrap(void)
{
    char *buf = malloc(DC_EVLOG_CONSOLE_BYTES);
    dc_evlog_console_read_t info;
    produce_to(C + 500);

    expect_true("after one wrap, cursor == oldest reads without loss",
                read_ok(500, buf, DC_EVLOG_CONSOLE_BYTES, &info) &&
                info.oldest_seq == 500 && info.start_seq == 500 &&
                info.lost_bytes == 0 && info.len == C);
    expect_true("after one wrap, cursor 0 loses exactly oldest bytes",
                read_ok(0, buf, DC_EVLOG_CONSOLE_BYTES, &info) &&
                info.lost_bytes == 500 && info.start_seq == 500);
    expect_true("cursor one below oldest loses exactly 1 byte",
                read_ok(499, buf, 16, &info) && info.lost_bytes == 1 &&
                info.start_seq == 500 && info.len == 16);
    // Ring index 0 now holds seq C; a read across it proves seq % C mapping.
    expect_true("read spanning the physical ring boundary maps correctly",
                read_ok(C - 10, buf, 20, &info) && info.start_seq == C - 10 &&
                info.len == 20);
    expect_true("future cursor rejected after wrap", future_rejected(C + 501));
    free(buf);
}

static void test_multiple_wraps(void)
{
    char *buf = malloc(DC_EVLOG_CONSOLE_BYTES);
    dc_evlog_console_read_t info;
    produce_to(3 * C + 777);
    uint64_t oldest = 2 * C + 777;

    expect_true("after multiple wraps, cursor 0 loss is exact",
                read_ok(0, buf, DC_EVLOG_CONSOLE_BYTES, &info) &&
                info.lost_bytes == oldest && info.start_seq == oldest &&
                info.len == C);
    expect_true("after multiple wraps, stale mid cursor loss is exact",
                read_ok(C + 3, buf, 64, &info) &&
                info.lost_bytes == oldest - (C + 3));
    expect_true("after multiple wraps, cursor == oldest is lossless",
                read_ok(oldest, buf, 64, &info) && info.lost_bytes == 0);
    expect_true("after multiple wraps, tail read is exact",
                read_ok(s_produced - 3, buf, 64, &info) && info.len == 3);
    free(buf);
}

static void test_chaining_reconstructs_retained_window(void)
{
    char *whole = malloc(DC_EVLOG_CONSOLE_BYTES);
    char chunk[1024];
    dc_evlog_console_read_t info;
    uint64_t cursor = model_oldest();
    uint64_t first = cursor;
    size_t total = 0;
    bool ok = true;
    int reads = 0;

    while (ok) {
        ok = read_ok(cursor, chunk, sizeof(chunk), &info) &&
             info.start_seq == cursor && info.lost_bytes == 0;
        if (!ok || info.len == 0) break;
        memcpy(whole + total, chunk, info.len);
        total += info.len;
        cursor = info.end_seq;
        reads++;
    }
    expect_true("chained reads have no gaps, duplicates or loss",
                ok && total == C && cursor == s_produced && reads == (int)(C / 1024));
    expect_true("chained reads reconstruct the retained window byte-exactly",
                bytes_match(whole, first, total));
    free(whole);
}

// Producer progress between independent reads: small steps, steps that
// overwrite part of the unread range, and steps larger than the whole ring.
static void test_producer_advancing_between_reads(void)
{
    static const uint64_t advances[] = {
        0, 1, 17, 1023, 1024, 1025, 5000, C - 1, C, C + 1, 3 * C + 5, 9, 0, 2 * C,
    };
    char chunk[700];
    dc_evlog_console_read_t info;
    uint64_t cursor = s_produced;
    uint64_t last_end = cursor;
    uint64_t received = 0;
    uint64_t lost = 0;
    uint64_t start_cursor = cursor;
    bool ok = true;

    for (size_t i = 0; ok && i < sizeof(advances) / sizeof(advances[0]); ++i) {
        produce(advances[i]);
        // Drain until caught up, as a polling consumer would.
        do {
            ok = read_ok(cursor, chunk, sizeof(chunk), &info) &&
                 info.start_seq >= cursor &&        // never rewinds
                 info.start_seq >= last_end &&      // never re-delivers a byte
                 (info.lost_bytes == 0) == (info.start_seq == cursor);
            received += info.len;
            lost += info.lost_bytes;
            last_end = info.end_seq;
            cursor = info.end_seq;
        } while (ok && info.len == sizeof(chunk));
    }
    expect_true("advancing producer: no hidden rewind or duplicate bytes", ok);
    expect_true("advancing producer: every byte is either received or counted lost",
                ok && received + lost == s_produced - start_cursor &&
                cursor == s_produced);
    expect_true("advancing producer: loss was actually exercised", lost > 0);
}

static void test_snapshot_apis_still_work(void)
{
    char *snap = malloc(DC_EVLOG_CONSOLE_BYTES + 1);
    size_t n = dc_evlog_console_snapshot(snap, DC_EVLOG_CONSOLE_BYTES + 1);
    expect_true("legacy atomic snapshot still returns the full ring", n == C);
    expect_true("legacy snapshot equals the cursor view of the same window",
                bytes_match(snap, model_oldest(), n));
    char first[64];
    dc_evlog_console_view_t view;
    size_t got = dc_evlog_console_snapshot_begin(&view, first, sizeof(first));
    expect_true("legacy snapshot view still reports write_seq",
                got == sizeof(first) && view.write_seq == s_produced &&
                bytes_match(first, model_oldest(), got));
    free(snap);
}

int main(void)
{
    dc_evlog_console_init();
    test_empty_ring();
    test_argument_validation();
    test_partial_ring();
    test_ring_exactly_full();
    test_single_wrap();
    test_multiple_wraps();
    test_chaining_reconstructs_retained_window();
    test_producer_advancing_between_reads();
    test_snapshot_apis_still_work();

    if (s_failures != 0)
        fprintf(stderr, "%d cursor test(s) failed\n", s_failures);
    return s_failures == 0 ? 0 : 1;
}
