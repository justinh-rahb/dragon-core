// Host tests for dc_logtail's transport-free contract layer, driven by real
// dc_evlog_console_read() results. The esp_http_server shell in dc_logtail.c
// only emits the response plan this layer builds; its integration is covered
// by the ESP-IDF compile test.

#include "dc_evlog.h"
#include "dc_logtail_contract.h"
#include "dc_logtail_limits.h"

#include "esp_log.h"

#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static int s_failures;
static vprintf_like_t s_log_hook;
static uint64_t s_produced;

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
    return (unsigned char)((seq * 197u + 3u) & 0xFFu);
}

static void produce(uint64_t n)
{
    for (uint64_t i = 0; i < n; ++i) {
        capture_log("%c", (int)byte_at(s_produced));
        s_produced++;
    }
}

static void expect_true(const char *name, bool condition)
{
    printf("[%s] %s\n", condition ? "PASS" : "FAIL", name);
    if (!condition) s_failures++;
}

static bool parses(const char *s, uint64_t expected)
{
    uint64_t v = 0xDEADBEEF;
    return dc_logtail_parse_u64(s, strlen(s), &v) && v == expected;
}

static bool rejects(const char *s)
{
    uint64_t v = 0xDEADBEEF;
    return !dc_logtail_parse_u64(s, strlen(s), &v) && v == 0xDEADBEEF;
}

static bool query_parses(const char *q, uint64_t expected)
{
    uint64_t v = 0xDEADBEEF;
    return dc_logtail_parse_query(q, &v) && v == expected;
}

static bool query_rejects(const char *q)
{
    uint64_t v = 0xDEADBEEF;
    return !dc_logtail_parse_query(q, &v) && v == 0xDEADBEEF;
}

static const char *header(const dc_logtail_response_t *r, const char *name)
{
    for (size_t i = 0; i < r->header_count; ++i)
        if (strcmp(r->headers[i].name, name) == 0) return r->headers[i].value;
    return NULL;
}

static bool header_u64(const dc_logtail_response_t *r, const char *name, uint64_t expected)
{
    const char *v = header(r, name);
    uint64_t got = 0;
    return v != NULL && dc_logtail_parse_u64(v, strlen(v), &got) && got == expected;
}

// Every 200 must carry all eight headers, each equal to the read it came from.
static bool ok_headers_match(const dc_logtail_response_t *r,
                             const dc_evlog_console_read_t *info, const char *boot)
{
    const char *b = header(r, DC_LOGTAIL_HDR_BOOT_ID);
    return r->header_count == 8 && b != NULL && strcmp(b, boot) == 0 &&
           header_u64(r, DC_LOGTAIL_HDR_CURSOR, info->cursor) &&
           header_u64(r, DC_LOGTAIL_HDR_OLDEST, info->oldest_seq) &&
           header_u64(r, DC_LOGTAIL_HDR_START, info->start_seq) &&
           header_u64(r, DC_LOGTAIL_HDR_END, info->end_seq) &&
           header_u64(r, DC_LOGTAIL_HDR_WRITE, info->write_seq) &&
           header_u64(r, DC_LOGTAIL_HDR_LOST, info->lost_bytes) &&
           header_u64(r, DC_LOGTAIL_HDR_CAPACITY, info->capacity);
}

static void test_cursor_parsing(void)
{
    expect_true("parse 0", parses("0", 0));
    expect_true("parse 1", parses("1", 1));
    expect_true("parse 16384", parses("16384", 16384));
    expect_true("parse UINT64_MAX", parses("18446744073709551615", UINT64_MAX));
    expect_true("parse UINT64_MAX - 1", parses("18446744073709551614", UINT64_MAX - 1));

    expect_true("reject UINT64_MAX + 1", rejects("18446744073709551616"));
    expect_true("reject 20-digit overflow", rejects("99999999999999999999"));
    expect_true("reject 21 digits", rejects("100000000000000000000"));
    expect_true("reject 2^64 * 10 wrap candidate", rejects("184467440737095516150"));
    expect_true("reject empty", rejects(""));
    expect_true("reject negative", rejects("-1"));
    expect_true("reject negative zero", rejects("-0"));
    expect_true("reject explicit plus", rejects("+1"));
    expect_true("reject leading space", rejects(" 1"));
    expect_true("reject trailing space", rejects("1 "));
    expect_true("reject trailing garbage", rejects("12abc"));
    expect_true("reject leading garbage", rejects("x12"));
    expect_true("reject hex", rejects("0x10"));
    expect_true("reject fraction", rejects("1.0"));
    expect_true("reject exponent", rejects("1e3"));
    expect_true("reject leading zero", rejects("01"));
    expect_true("reject double zero", rejects("00"));
    expect_true("reject trailing newline", rejects("5\n"));
    uint64_t v = 7;
    expect_true("reject embedded NUL (length-bounded)",
                !dc_logtail_parse_u64("5\0" "5", 3, &v) && v == 7);
    expect_true("reject NULL string", !dc_logtail_parse_u64(NULL, 1, &v));
    expect_true("reject NULL out", !dc_logtail_parse_u64("1", 1, NULL));
}

static void test_query_parsing(void)
{
    expect_true("query cursor=0", query_parses("cursor=0", 0));
    expect_true("query cursor=12345", query_parses("cursor=12345", 12345));
    expect_true("query cursor=UINT64_MAX",
                query_parses("cursor=18446744073709551615", UINT64_MAX));

    expect_true("query empty rejected", query_rejects(""));
    expect_true("query missing value rejected", query_rejects("cursor="));
    expect_true("query bare key rejected", query_rejects("cursor"));
    expect_true("query wrong case rejected", query_rejects("Cursor=5"));
    expect_true("query legacy after= rejected", query_rejects("after=5"));
    expect_true("query duplicate cursor rejected", query_rejects("cursor=5&cursor=6"));
    expect_true("query extra param rejected", query_rejects("cursor=5&max=10"));
    expect_true("query param before cursor rejected", query_rejects("x=1&cursor=5"));
    expect_true("query trailing separator rejected", query_rejects("cursor=5&"));
    expect_true("query semicolon separator rejected", query_rejects("cursor=5;x=1"));
    expect_true("query percent-encoded digit rejected", query_rejects("cursor=%35"));
    expect_true("query negative rejected", query_rejects("cursor=-1"));
    expect_true("query overflow rejected", query_rejects("cursor=18446744073709551616"));
    expect_true("query signed rejected", query_rejects("cursor=+5"));
    expect_true("query trailing garbage rejected", query_rejects("cursor=5x"));
    expect_true("query double equals rejected", query_rejects("cursor==5"));

    char longq[DC_LOGTAIL_QUERY_MAX + 2];
    memset(longq, '0', sizeof(longq) - 1);
    memcpy(longq, "cursor=", 7);
    longq[sizeof(longq) - 1] = '\0';
    expect_true("query longer than limit rejected", query_rejects(longq));
    uint64_t v;
    expect_true("query NULL rejected", !dc_logtail_parse_query(NULL, &v));
}

static void test_u64_formatting(void)
{
    static const uint64_t values[] = {
        0, 1, 9, 10, 99, 100, 16384, 4294967295u, 4294967296u,
        1000000000000000000u, 9999999999999999999u, 10000000000000000000u,
        UINT64_MAX - 1, UINT64_MAX,
    };
    char buf[DC_LOGTAIL_U64_CHARS];
    bool ok = true;
    for (size_t i = 0; i < sizeof(values) / sizeof(values[0]); ++i) {
        size_t n = dc_logtail_format_u64(values[i], buf);
        uint64_t back = 0;
        ok &= n == strlen(buf) && dc_logtail_parse_u64(buf, n, &back) && back == values[i];
    }
    expect_true("u64 format/parse round-trips across the full range", ok);
    dc_logtail_format_u64(UINT64_MAX, buf);
    expect_true("UINT64_MAX formats exactly", strcmp(buf, "18446744073709551615") == 0);
    dc_logtail_format_u64(0, buf);
    expect_true("0 formats as \"0\"", strcmp(buf, "0") == 0);
}

static void test_boot_id_validation(void)
{
    char max_id[DC_LOGTAIL_BOOT_ID_MAX + 2];
    memset(max_id, 'a', sizeof(max_id));
    max_id[DC_LOGTAIL_BOOT_ID_MAX] = '\0';
    expect_true("boot id: uuid-like accepted",
                dc_logtail_boot_id_valid("3f2a9c1e-77b0-4d2e-9a51-0c6f1e2d3b4a"));
    expect_true("boot id: dotted/colon accepted", dc_logtail_boot_id_valid("bench.7:boot_12"));
    expect_true("boot id: max length accepted", dc_logtail_boot_id_valid(max_id));
    max_id[DC_LOGTAIL_BOOT_ID_MAX] = 'a';
    max_id[DC_LOGTAIL_BOOT_ID_MAX + 1] = '\0';
    expect_true("boot id: over max length rejected", !dc_logtail_boot_id_valid(max_id));
    expect_true("boot id: NULL rejected", !dc_logtail_boot_id_valid(NULL));
    expect_true("boot id: empty rejected", !dc_logtail_boot_id_valid(""));
    expect_true("boot id: space rejected", !dc_logtail_boot_id_valid("a b"));
    expect_true("boot id: CRLF header injection rejected",
                !dc_logtail_boot_id_valid("abc\r\nSet-Cookie: x"));
    expect_true("boot id: non-ASCII rejected", !dc_logtail_boot_id_valid("boot\xc3\xa9"));
    expect_true("boot id: quote rejected", !dc_logtail_boot_id_valid("a\"b"));
}

static void test_auth_policy(void)
{
    expect_true("auth: callback only -> callback mode",
                dc_logtail_auth_mode(true, false) == DC_LOGTAIL_AUTH_CALLBACK);
    expect_true("auth: explicit opt-in only -> open mode",
                dc_logtail_auth_mode(false, true) == DC_LOGTAIL_AUTH_OPEN);
    expect_true("auth: NULL callback without opt-in is invalid, not public",
                dc_logtail_auth_mode(false, false) == DC_LOGTAIL_AUTH_INVALID);
    expect_true("auth: callback plus opt-in is ambiguous and invalid",
                dc_logtail_auth_mode(true, true) == DC_LOGTAIL_AUTH_INVALID);

    expect_true("auth: callback allow permits",
                dc_logtail_auth_permits(DC_LOGTAIL_AUTH_CALLBACK, true));
    expect_true("auth: callback deny refuses",
                !dc_logtail_auth_permits(DC_LOGTAIL_AUTH_CALLBACK, false));
    expect_true("auth: open permits without callback",
                dc_logtail_auth_permits(DC_LOGTAIL_AUTH_OPEN, false));
    expect_true("auth: invalid mode refuses even with a true result",
                !dc_logtail_auth_permits(DC_LOGTAIL_AUTH_INVALID, true));
    expect_true("auth: out-of-range mode refuses",
                !dc_logtail_auth_permits((dc_logtail_auth_mode_t)42, true));
}

static void test_error_responses_carry_no_metadata(void)
{
    dc_logtail_response_t r;
    dc_logtail_build_error_response(DC_LOGTAIL_ERR_FORBIDDEN, &r);
    expect_true("403: status and JSON body",
                strcmp(r.status, "403 Forbidden") == 0 &&
                strcmp(r.content_type, "application/json") == 0 &&
                r.body != NULL && r.body_len == strlen(r.body) &&
                strstr(r.body, "authorization required") != NULL);
    expect_true("403: no ring or boot metadata", r.header_count == 0);

    dc_logtail_build_error_response(DC_LOGTAIL_ERR_BAD_REQUEST, &r);
    expect_true("400: status and JSON body",
                strcmp(r.status, "400 Bad Request") == 0 &&
                r.body != NULL && r.body_len == strlen(r.body) &&
                strstr(r.body, "invalid cursor") != NULL);
    expect_true("400: no ring or boot metadata", r.header_count == 0);

    dc_logtail_build_read_response(DC_EVLOG_CONSOLE_READ_OK, NULL, "boot", &r);
    expect_true("NULL info maps to 500, never a fake 200",
                strcmp(r.status, "500 Internal Server Error") == 0 &&
                r.header_count == 0 && r.body != NULL);
    dc_evlog_console_read_t info = {0};
    dc_logtail_build_read_response(DC_EVLOG_CONSOLE_READ_INVALID_ARG, &info, "boot", &r);
    expect_true("INVALID_ARG maps to 500 without metadata",
                strcmp(r.status, "500 Internal Server Error") == 0 &&
                r.header_count == 0);
}

static void test_empty_read_response(void)
{
    char payload[DC_LOGTAIL_MAX_PAYLOAD];
    dc_evlog_console_read_t info;
    dc_logtail_response_t r;
    const char *boot = "boot-A";
    dc_evlog_console_read_status_t st =
        dc_evlog_console_read(0, payload, sizeof(payload), &info);
    dc_logtail_build_read_response(st, &info, boot, &r);
    expect_true("empty valid read: 200 octet-stream with zero-length payload",
                strcmp(r.status, "200 OK") == 0 &&
                strcmp(r.content_type, "application/octet-stream") == 0 &&
                r.body == NULL && r.body_len == 0);
    expect_true("empty valid read: complete metadata, next cursor unchanged",
                ok_headers_match(&r, &info, boot) &&
                header_u64(&r, DC_LOGTAIL_HDR_END, 0));
    expect_true("boot id is propagated verbatim from the product",
                header(&r, DC_LOGTAIL_HDR_BOOT_ID) == boot);
}

static void test_raw_payload_preserved(void)
{
    char payload[DC_LOGTAIL_MAX_PAYLOAD];
    dc_evlog_console_read_t info;
    dc_logtail_response_t r;
    produce(300);   // includes NUL, CR, LF and bytes 0x80-0xFF
    bool has_nul = false, has_high = false, has_cr = false;
    for (uint64_t s = 0; s < 300; ++s) {
        has_nul |= byte_at(s) == 0x00;
        has_high |= byte_at(s) >= 0x80;
        has_cr |= byte_at(s) == '\r';
    }
    expect_true("fixture covers NUL, CR and non-UTF-8 bytes", has_nul && has_high && has_cr);

    dc_evlog_console_read_status_t st =
        dc_evlog_console_read(0, payload, sizeof(payload), &info);
    dc_logtail_build_read_response(st, &info, "boot-A", &r);
    bool exact = r.body == NULL && r.body_len == 300;
    for (size_t i = 0; exact && i < r.body_len; ++i)
        exact = (unsigned char)payload[i] == byte_at(i);
    expect_true("payload bytes are exactly the ring bytes, unmodified", exact);
    expect_true("body length equals end_seq - start_seq",
                r.body_len == info.end_seq - info.start_seq &&
                ok_headers_match(&r, &info, "boot-A"));
}

static void test_cap_and_stale_cursor(void)
{
    char payload[DC_LOGTAIL_MAX_PAYLOAD];
    dc_evlog_console_read_t info;
    dc_logtail_response_t r;

    expect_true("payload cap does not exceed ring capacity",
                DC_LOGTAIL_MAX_PAYLOAD <= DC_EVLOG_CONSOLE_BYTES);

    produce(DC_EVLOG_CONSOLE_BYTES * 2 + 123);
    uint64_t oldest = s_produced - DC_EVLOG_CONSOLE_BYTES;

    dc_evlog_console_read_status_t st =
        dc_evlog_console_read(5, payload, sizeof(payload), &info);
    dc_logtail_build_read_response(st, &info, "boot-A", &r);
    expect_true("stale cursor: 200 with exact lost bytes and start at oldest",
                strcmp(r.status, "200 OK") == 0 &&
                header_u64(&r, DC_LOGTAIL_HDR_LOST, oldest - 5) &&
                header_u64(&r, DC_LOGTAIL_HDR_START, oldest) &&
                header_u64(&r, DC_LOGTAIL_HDR_OLDEST, oldest) &&
                ok_headers_match(&r, &info, "boot-A"));
    expect_true("response is capped at DC_LOGTAIL_MAX_PAYLOAD",
                r.body_len == DC_LOGTAIL_MAX_PAYLOAD &&
                info.end_seq - info.start_seq == DC_LOGTAIL_MAX_PAYLOAD &&
                info.end_seq < info.write_seq);
    bool exact = true;
    for (size_t i = 0; exact && i < r.body_len; ++i)
        exact = (unsigned char)payload[i] == byte_at(oldest + i);
    expect_true("capped payload bytes match [start_seq, end_seq)", exact);
}

static void test_future_cursor_response(void)
{
    char payload[DC_LOGTAIL_MAX_PAYLOAD];
    dc_evlog_console_read_t info;
    dc_logtail_response_t r;
    uint64_t future = s_produced + 1;
    dc_evlog_console_read_status_t st =
        dc_evlog_console_read(future, payload, sizeof(payload), &info);
    dc_logtail_build_read_response(st, &info, "boot-A", &r);
    expect_true("future cursor: 409 with JSON error body",
                strcmp(r.status, "409 Conflict") == 0 &&
                strcmp(r.content_type, "application/json") == 0 &&
                r.body != NULL && r.body_len == strlen(r.body));
    expect_true("future cursor: resync metadata present, no range headers",
                r.header_count == 5 &&
                header_u64(&r, DC_LOGTAIL_HDR_CURSOR, future) &&
                header_u64(&r, DC_LOGTAIL_HDR_WRITE, s_produced) &&
                header_u64(&r, DC_LOGTAIL_HDR_OLDEST, s_produced - DC_EVLOG_CONSOLE_BYTES) &&
                header_u64(&r, DC_LOGTAIL_HDR_CAPACITY, DC_EVLOG_CONSOLE_BYTES) &&
                strcmp(header(&r, DC_LOGTAIL_HDR_BOOT_ID), "boot-A") == 0 &&
                header(&r, DC_LOGTAIL_HDR_START) == NULL &&
                header(&r, DC_LOGTAIL_HDR_END) == NULL &&
                header(&r, DC_LOGTAIL_HDR_LOST) == NULL);
}

static void test_headers_cover_full_u64_range(void)
{
    dc_evlog_console_read_t info = {
        .cursor = UINT64_MAX - 5000,
        .oldest_seq = UINT64_MAX - 4000,
        .start_seq = UINT64_MAX - 4000,
        .end_seq = UINT64_MAX - 2976,
        .write_seq = UINT64_MAX,
        .lost_bytes = 1000,
        .len = 1024,
        .capacity = DC_EVLOG_CONSOLE_BYTES,
    };
    dc_logtail_response_t r;
    dc_logtail_build_read_response(DC_EVLOG_CONSOLE_READ_OK, &info, "b", &r);
    expect_true("headers carry values near UINT64_MAX exactly",
                ok_headers_match(&r, &info, "b") &&
                strcmp(header(&r, DC_LOGTAIL_HDR_WRITE), "18446744073709551615") == 0);

    bool distinct = true;
    for (size_t i = 0; i < r.header_count; ++i)
        for (size_t j = i + 1; j < r.header_count; ++j)
            distinct &= strcmp(r.headers[i].name, r.headers[j].name) != 0;
    expect_true("header names are distinct", distinct);
    expect_true("200 uses exactly ESP-IDF's default 8 response-header slots",
                r.header_count == DC_LOGTAIL_MAX_HEADERS && DC_LOGTAIL_MAX_HEADERS == 8);
}

int main(void)
{
    dc_evlog_console_init();
    test_cursor_parsing();
    test_query_parsing();
    test_u64_formatting();
    test_boot_id_validation();
    test_auth_policy();
    test_error_responses_carry_no_metadata();
    test_empty_read_response();
    test_raw_payload_preserved();
    test_cap_and_stale_cursor();
    test_future_cursor_response();
    test_headers_cover_full_u64_range();

    if (s_failures != 0)
        fprintf(stderr, "%d logtail test(s) failed\n", s_failures);
    return s_failures == 0 ? 0 : 1;
}
