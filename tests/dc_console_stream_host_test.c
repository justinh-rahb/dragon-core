#include "dc_evlog.h"
#include "dc_portal_console_stream.h"

#include "esp_log.h"

#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int s_failures;
static vprintf_like_t s_log_hook;

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

static void expect_true(const char *name, bool condition)
{
    printf("[%s] %s\n", condition ? "PASS" : "FAIL", name);
    if (!condition) s_failures++;
}

static void fill_console_ring(void)
{
    char line[200];
    memset(line, 'F', sizeof(line) - 1);
    line[sizeof(line) - 1] = '\0';
    for (size_t i = 0; i < (DC_EVLOG_CONSOLE_BYTES / (sizeof(line) - 1)) + 2; ++i)
        capture_log("%s", line);
}

static void overwrite_more_than_first_chunk(void)
{
    char line[200];
    memset(line, 'R', sizeof(line) - 1);
    line[sizeof(line) - 1] = '\0';
    for (int i = 0; i < 6; ++i) capture_log("%s", line);
}

typedef struct {
    size_t calls;
    size_t data_chunks;
    bool terminated;
    bool saw_rotation_marker;
    bool overwrite_after_first_chunk;
    size_t fail_at_call;
    esp_err_t failure_code;
} send_trace_t;

static esp_err_t trace_send(void *ctx, const char *data, size_t len)
{
    send_trace_t *trace = ctx;
    trace->calls++;
    if (trace->fail_at_call != 0 && trace->calls == trace->fail_at_call)
        return trace->failure_code;
    if (data == NULL && len == 0) {
        trace->terminated = true;
        return ESP_OK;
    }
    trace->data_chunks++;
    if (len == strlen("\n[console snapshot rotated while streaming]\n") &&
        memcmp(data, "\n[console snapshot rotated while streaming]\n", len) == 0)
        trace->saw_rotation_marker = true;
    if (trace->overwrite_after_first_chunk && trace->data_chunks == 1)
        overwrite_more_than_first_chunk();
    return ESP_OK;
}

static void test_empty_console_terminates(void)
{
    send_trace_t trace = {0};
    esp_err_t err = dc_portal_console_stream(&trace, trace_send);

    expect_true("empty stream returns success", err == ESP_OK);
    expect_true("empty stream sends no data", trace.data_chunks == 0);
    expect_true("empty stream is explicitly terminated", trace.terminated);
}

static void test_atomic_snapshot_compatibility(void)
{
    char *snapshot = malloc(DC_EVLOG_CONSOLE_BYTES + 1);
    expect_true("atomic snapshot allocation", snapshot != NULL);
    if (snapshot == NULL) return;

    size_t n = dc_evlog_console_snapshot(snapshot, DC_EVLOG_CONSOLE_BYTES + 1);
    expect_true("atomic snapshot returns complete full ring",
                n == DC_EVLOG_CONSOLE_BYTES);
    expect_true("atomic snapshot is NUL terminated", snapshot[n] == '\0');

    char small[17];
    size_t small_n = dc_evlog_console_snapshot(small, sizeof(small));
    expect_true("atomic snapshot honors caller capacity", small_n == sizeof(small) - 1);
    expect_true("bounded atomic snapshot is NUL terminated", small[small_n] == '\0');
    free(snapshot);
}

static void test_rotation_is_graceful(void)
{
    send_trace_t trace = { .overwrite_after_first_chunk = true };
    esp_err_t err = dc_portal_console_stream(&trace, trace_send);

    expect_true("rotation stream returns success", err == ESP_OK);
    expect_true("first coherent data chunk was sent", trace.data_chunks >= 2);
    expect_true("rotation marker was sent", trace.saw_rotation_marker);
    expect_true("rotated stream was explicitly terminated", trace.terminated);
}

static void test_snapshot_reports_invalidation(void)
{
    char first[1024];
    char later[1024];
    dc_evlog_console_view_t view;
    size_t first_n = dc_evlog_console_snapshot_begin(&view, first, sizeof(first));
    size_t later_n = 99;

    overwrite_more_than_first_chunk();
    bool valid = dc_evlog_console_snapshot_read(&view, first_n, later,
                                                 sizeof(later), &later_n);
    expect_true("full-ring first chunk is 1024 bytes", first_n == sizeof(first));
    expect_true("overwritten unread snapshot reports invalidation", !valid);
    expect_true("invalidated read reports zero bytes", later_n == 0);
}

static void test_send_failure_propagates(void)
{
    const esp_err_t send_error = 0x777;
    send_trace_t trace = { .fail_at_call = 1, .failure_code = send_error };
    esp_err_t err = dc_portal_console_stream(&trace, trace_send);

    expect_true("send failure is propagated", err == send_error);
    expect_true("failed transport is not falsely terminated", !trace.terminated);

    send_trace_t marker_failure = {
        .overwrite_after_first_chunk = true,
        .fail_at_call = 2,
        .failure_code = send_error,
    };
    err = dc_portal_console_stream(&marker_failure, trace_send);
    expect_true("rotation marker send failure is propagated", err == send_error);
    expect_true("failed marker transport is not falsely terminated",
                !marker_failure.terminated);
}

int main(void)
{
    dc_evlog_console_init();
    test_empty_console_terminates();
    fill_console_ring();
    test_atomic_snapshot_compatibility();
    test_snapshot_reports_invalidation();
    test_rotation_is_graceful();
    test_send_failure_propagates();

    if (s_failures != 0)
        fprintf(stderr, "%d console test(s) failed\n", s_failures);
    return s_failures == 0 ? 0 : 1;
}
