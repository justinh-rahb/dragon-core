// Handler-level regression tests for dc_logtail.c, run against a minimal fake
// of esp_http_server (tests/stubs_httpd) that mirrors ESP-IDF v5.3: response
// status, type and headers are stored by pointer, set_hdr fails once
// max_resp_headers slots are used, installed headers cannot be removed, and
// the wire is written only by httpd_resp_send().
//
// Invariant under test: a 200 or 409 is never transmitted with incomplete
// evidence metadata, for every max_resp_headers value, and the optional
// Cache-Control header can never displace a required one.

#include "dc_evlog.h"
#include "dc_logtail.h"

#include "esp_http_server.h"
#include "esp_log.h"

#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_FAKE_HDRS 16

struct httpd_req {
    // request side
    const char *query;          // NULL: no query string
    bool auth_ok;
    size_t max_resp_headers;
    // stored (not yet transmitted) response state
    const char *status;
    const char *type;
    const char *hdr_field[MAX_FAKE_HDRS];
    const char *hdr_value[MAX_FAKE_HDRS];
    size_t hdr_count;
    // what reached the wire
    int send_calls;
    char wire_status[64];
    char wire_type[64];
    size_t wire_hdr_count;
    char wire_field[MAX_FAKE_HDRS][40];
    char wire_value[MAX_FAKE_HDRS][80];
    unsigned char wire_body[2 * DC_LOGTAIL_MAX_PAYLOAD];
    size_t wire_body_len;
};

static int s_failures;
static vprintf_like_t s_log_hook;
static uint64_t s_produced;
static httpd_uri_t s_route;
static int s_registrations;

// ---- esp_log hook stub --------------------------------------------------
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
    return (unsigned char)((seq * 61u + 11u) & 0xFFu);
}

static void produce(uint64_t n)
{
    for (uint64_t i = 0; i < n; ++i) {
        capture_log("%c", (int)byte_at(s_produced));
        s_produced++;
    }
}

// ---- esp_http_server fake ----------------------------------------------
esp_err_t httpd_register_uri_handler(httpd_handle_t handle, const httpd_uri_t *uri)
{
    if (handle == NULL || uri == NULL) return ESP_ERR_INVALID_ARG;
    s_route = *uri;
    s_registrations++;
    return ESP_OK;
}

esp_err_t httpd_resp_set_status(httpd_req_t *r, const char *status)
{
    if (r == NULL || status == NULL) return ESP_ERR_INVALID_ARG;
    r->status = status;
    return ESP_OK;
}

esp_err_t httpd_resp_set_type(httpd_req_t *r, const char *type)
{
    if (r == NULL || type == NULL) return ESP_ERR_INVALID_ARG;
    r->type = type;
    return ESP_OK;
}

esp_err_t httpd_resp_set_hdr(httpd_req_t *r, const char *field, const char *value)
{
    if (r == NULL || field == NULL || value == NULL) return ESP_ERR_INVALID_ARG;
    if (r->hdr_count >= r->max_resp_headers || r->hdr_count >= MAX_FAKE_HDRS)
        return ESP_ERR_HTTPD_RESP_HDR;
    r->hdr_field[r->hdr_count] = field;   // pointer storage, as in ESP-IDF
    r->hdr_value[r->hdr_count] = value;
    r->hdr_count++;
    return ESP_OK;
}

esp_err_t httpd_resp_send(httpd_req_t *r, const char *buf, ssize_t buf_len)
{
    r->send_calls++;
    // Values are dereferenced only now, exactly like ESP-IDF, which also
    // proves the header storage outlives the send.
    snprintf(r->wire_status, sizeof(r->wire_status), "%s", r->status ? r->status : "200 OK");
    snprintf(r->wire_type, sizeof(r->wire_type), "%s", r->type ? r->type : "text/html");
    r->wire_hdr_count = r->hdr_count;
    for (size_t i = 0; i < r->hdr_count; ++i) {
        snprintf(r->wire_field[i], sizeof(r->wire_field[i]), "%s", r->hdr_field[i]);
        snprintf(r->wire_value[i], sizeof(r->wire_value[i]), "%s", r->hdr_value[i]);
    }
    size_t len = buf_len < 0 ? strlen(buf) : (size_t)buf_len;
    if (len > sizeof(r->wire_body)) return ESP_FAIL;
    if (len > 0) memcpy(r->wire_body, buf, len);
    r->wire_body_len = len;
    return ESP_OK;
}

size_t httpd_req_get_url_query_len(httpd_req_t *r)
{
    return r->query ? strlen(r->query) : 0;
}

esp_err_t httpd_req_get_url_query_str(httpd_req_t *r, char *buf, size_t buf_len)
{
    if (r->query == NULL) return ESP_ERR_NOT_FOUND;
    size_t n = strlen(r->query);
    size_t copy = n < buf_len - 1 ? n : buf_len - 1;
    memcpy(buf, r->query, copy);
    buf[copy] = '\0';
    return n >= buf_len ? ESP_ERR_HTTPD_RESULT_TRUNC : ESP_OK;
}

// ---- helpers ------------------------------------------------------------
static bool product_authorize(httpd_req_t *req, void *ctx)
{
    (void)ctx;
    return req->auth_ok;
}

static void expect_true(const char *name, bool condition)
{
    printf("[%s] %s\n", condition ? "PASS" : "FAIL", name);
    if (!condition) s_failures++;
}

static const char *const REQUIRED_200[] = {
    "Dragon-Log-Boot-Id", "Dragon-Log-Cursor", "Dragon-Log-Oldest-Seq",
    "Dragon-Log-Start-Seq", "Dragon-Log-End-Seq", "Dragon-Log-Write-Seq",
    "Dragon-Log-Lost-Bytes", "Dragon-Log-Capacity",
};
static const char *const REQUIRED_409[] = {
    "Dragon-Log-Boot-Id", "Dragon-Log-Cursor", "Dragon-Log-Oldest-Seq",
    "Dragon-Log-Write-Seq", "Dragon-Log-Capacity",
};
#define N_ELEMS(a) (sizeof(a) / sizeof((a)[0]))

static const char *wire_hdr(const httpd_req_t *r, const char *name)
{
    for (size_t i = 0; i < r->wire_hdr_count; ++i)
        if (strcmp(r->wire_field[i], name) == 0) return r->wire_value[i];
    return NULL;
}

static bool wire_u64(const httpd_req_t *r, const char *name, uint64_t *out)
{
    const char *v = wire_hdr(r, name);
    if (v == NULL || *v == '\0') return false;
    uint64_t acc = 0;
    for (const char *p = v; *p; ++p) {
        if (*p < '0' || *p > '9') return false;
        acc = acc * 10u + (uint64_t)(*p - '0');
    }
    *out = acc;
    return true;
}

static bool has_all(const httpd_req_t *r, const char *const *names, size_t n)
{
    for (size_t i = 0; i < n; ++i) {
        const char *v = wire_hdr(r, names[i]);
        if (v == NULL || *v == '\0') return false;
    }
    return true;
}

static size_t evidence_hdr_count(const httpd_req_t *r)
{
    size_t n = 0;
    for (size_t i = 0; i < r->wire_hdr_count; ++i)
        n += strncmp(r->wire_field[i], "Dragon-Log-", 11) == 0;
    return n;
}

static bool is_status(const httpd_req_t *r, const char *prefix)
{
    return strncmp(r->wire_status, prefix, strlen(prefix)) == 0;
}

static httpd_req_t *run(const char *query, bool auth_ok, size_t max_hdrs)
{
    httpd_req_t *r = calloc(1, sizeof(*r));
    r->query = query;
    r->auth_ok = auth_ok;
    r->max_resp_headers = max_hdrs;
    (void)s_route.handler(r);
    return r;
}

// The core invariant, checked on every transmitted response.
static bool evidence_complete_or_not_nominal(const httpd_req_t *r)
{
    if (is_status(r, "200")) return has_all(r, REQUIRED_200, N_ELEMS(REQUIRED_200));
    if (is_status(r, "409")) return has_all(r, REQUIRED_409, N_ELEMS(REQUIRED_409));
    return true;
}

// ---- tests --------------------------------------------------------------
static void test_registration(void)
{
    dc_logtail_config_t zero = {0};
    expect_true("register: zero config rejected (NULL auth is not public)",
                dc_logtail_register((httpd_handle_t)1, &zero) == ESP_ERR_INVALID_ARG &&
                s_registrations == 0);
    dc_logtail_config_t both = { .boot_id = "b", .authorize = product_authorize,
                                 .allow_unauthenticated = true };
    expect_true("register: callback plus opt-in rejected",
                dc_logtail_register((httpd_handle_t)1, &both) == ESP_ERR_INVALID_ARG);
    dc_logtail_config_t bad_id = { .boot_id = "bad id", .authorize = product_authorize };
    expect_true("register: invalid boot id rejected",
                dc_logtail_register((httpd_handle_t)1, &bad_id) == ESP_ERR_INVALID_ARG);

    char boot[] = "boot-7f3a";
    dc_logtail_config_t cfg = { .boot_id = boot, .authorize = product_authorize };
    expect_true("register: valid config registers GET route",
                dc_logtail_register((httpd_handle_t)1, &cfg) == ESP_OK &&
                s_registrations == 1 && s_route.method == HTTP_GET &&
                strcmp(s_route.uri, "/api/v1/system/log") == 0);
    boot[0] = 'X';   // caller storage changes after registration
    cfg.boot_id = "boot-7f3a";
    expect_true("register: identical config accepted again (server restart)",
                dc_logtail_register((httpd_handle_t)2, &cfg) == ESP_OK);
    dc_logtail_config_t other = { .boot_id = "boot-other", .authorize = product_authorize };
    expect_true("register: different boot id mid-boot rejected",
                dc_logtail_register((httpd_handle_t)1, &other) == ESP_ERR_INVALID_STATE);
    dc_logtail_config_t open = { .boot_id = "boot-7f3a", .allow_unauthenticated = true };
    expect_true("register: different auth mode mid-boot rejected",
                dc_logtail_register((httpd_handle_t)1, &open) == ESP_ERR_INVALID_STATE);
}

static void test_default_slots_complete_200(void)
{
    produce(300);
    httpd_req_t *r = run("cursor=0", true, 8);
    uint64_t start = 0, end = 0, write = 0, lost = 1, cap = 0;
    bool nums = wire_u64(r, "Dragon-Log-Start-Seq", &start) &&
                wire_u64(r, "Dragon-Log-End-Seq", &end) &&
                wire_u64(r, "Dragon-Log-Write-Seq", &write) &&
                wire_u64(r, "Dragon-Log-Lost-Bytes", &lost) &&
                wire_u64(r, "Dragon-Log-Capacity", &cap);
    bool body = r->wire_body_len == 300;
    for (size_t i = 0; body && i < r->wire_body_len; ++i) body = r->wire_body[i] == byte_at(i);
    expect_true("max_resp_headers=8: single send, 200, octet-stream",
                r->send_calls == 1 && is_status(r, "200") &&
                strcmp(r->wire_type, "application/octet-stream") == 0);
    expect_true("max_resp_headers=8: all eight evidence headers, nothing else",
                r->wire_hdr_count == 8 && has_all(r, REQUIRED_200, 8) &&
                wire_hdr(r, "Cache-Control") == NULL);
    expect_true("max_resp_headers=8: boot id is the latched copy",
                strcmp(wire_hdr(r, "Dragon-Log-Boot-Id"), "boot-7f3a") == 0);
    expect_true("max_resp_headers=8: metadata matches body byte-exactly",
                nums && body && start == 0 && end == 300 && write == 300 &&
                lost == 0 && cap == DC_EVLOG_CONSOLE_BYTES &&
                end - start == r->wire_body_len);
    free(r);
}

static void test_spare_slot_adds_cache_control_last(void)
{
    httpd_req_t *r = run("cursor=0", true, 9);
    expect_true("max_resp_headers=9: 200 with eight evidence headers first",
                is_status(r, "200") && r->wire_hdr_count == 9 && has_all(r, REQUIRED_200, 8));
    bool order = true;
    for (size_t i = 0; i < 8; ++i) order &= strncmp(r->wire_field[i], "Dragon-Log-", 11) == 0;
    expect_true("max_resp_headers=9: optional Cache-Control only in the spare slot",
                order && strcmp(r->wire_field[8], "Cache-Control") == 0 &&
                strcmp(r->wire_value[8], "no-store") == 0);
    free(r);
}

static void test_insufficient_slots_fail_closed(void)
{
    bool all_ok = true;
    for (size_t k = 0; k < 8; ++k) {
        httpd_req_t *r = run("cursor=0", true, k);
        bool ok = r->send_calls == 1 &&
                  strcmp(r->wire_status, "500 Internal Server Error") == 0 &&
                  strcmp(r->wire_type, "application/json") == 0 &&
                  r->wire_body_len > 0 && r->wire_body[0] == '{' &&
                  strstr((const char *)r->wire_body, "\"ok\":false") != NULL &&
                  r->wire_hdr_count == k &&
                  wire_hdr(r, "Cache-Control") == NULL;
        if (!ok) printf("  max_resp_headers=%zu: status='%s' hdrs=%zu sends=%d\n",
                        k, r->wire_status, r->wire_hdr_count, r->send_calls);
        all_ok &= ok;
        free(r);
    }
    expect_true("max_resp_headers 0..7: valid read is a deterministic 500, never 200", all_ok);
}

static void test_future_cursor_slots(void)
{
    char q[48];
    snprintf(q, sizeof(q), "cursor=%llu", (unsigned long long)(s_produced + 1));
    httpd_req_t *r = run(q, true, 8);
    bool first_five = r->wire_hdr_count == 6;
    for (size_t i = 0; first_five && i < 5; ++i)
        first_five = strncmp(r->wire_field[i], "Dragon-Log-", 11) == 0;
    expect_true("409 at max_resp_headers=8: five resync headers, JSON body",
                r->send_calls == 1 && is_status(r, "409") &&
                evidence_hdr_count(r) == 5 && has_all(r, REQUIRED_409, 5) &&
                strcmp(r->wire_type, "application/json") == 0);
    expect_true("409 at max_resp_headers=8: optional Cache-Control only after evidence",
                first_five && strcmp(r->wire_field[5], "Cache-Control") == 0);
    free(r);
    r = run(q, true, 5);
    expect_true("409 at max_resp_headers=5: still complete",
                is_status(r, "409") && has_all(r, REQUIRED_409, 5));
    free(r);
    bool all_ok = true;
    for (size_t k = 0; k < 5; ++k) {
        r = run(q, true, k);
        all_ok &= r->send_calls == 1 &&
                  strcmp(r->wire_status, "500 Internal Server Error") == 0 &&
                  r->wire_hdr_count == k;
        free(r);
    }
    expect_true("409 at max_resp_headers 0..4: deterministic 500, never 409", all_ok);
}

static void test_rejections_need_no_slots(void)
{
    httpd_req_t *r = run("cursor=0", false, 0);
    expect_true("403 needs no header slots and leaks no metadata",
                r->send_calls == 1 && is_status(r, "403") && r->wire_hdr_count == 0);
    free(r);
    r = run("cursor=01", true, 0);
    expect_true("400 needs no header slots", r->send_calls == 1 &&
                is_status(r, "400") && r->wire_hdr_count == 0);
    free(r);
    r = run(NULL, true, 8);
    expect_true("missing query is 400 without evidence metadata",
                is_status(r, "400") && evidence_hdr_count(r) == 0);
    free(r);
    r = run("cursor=00000000000000000000000000000000000000000000000000000000000000001",
            true, 8);
    expect_true("over-long query is 400 without evidence metadata",
                is_status(r, "400") && evidence_hdr_count(r) == 0);
    free(r);
}

static void test_invariant_sweep(void)
{
    char future[48];
    snprintf(future, sizeof(future), "cursor=%llu", (unsigned long long)(s_produced + 1));
    const char *queries[] = { "cursor=0", "cursor=150", future, "cursor=x", NULL };
    bool all_ok = true;
    int responses = 0;
    for (size_t q = 0; q < N_ELEMS(queries); ++q)
        for (size_t k = 0; k <= 10; ++k)
            for (int auth = 0; auth <= 1; ++auth) {
                httpd_req_t *r = run(queries[q], auth != 0, k);
                all_ok &= r->send_calls == 1 && evidence_complete_or_not_nominal(r);
                responses++;
                free(r);
            }
    printf("  swept %d responses\n", responses);
    expect_true("sweep: no 200/409 ever transmitted with incomplete metadata", all_ok);
}

int main(void)
{
    dc_evlog_console_init();
    test_registration();
    test_default_slots_complete_200();
    test_spare_slot_adds_cache_control_last();
    test_insufficient_slots_fail_closed();
    test_future_cursor_slots();
    test_rejections_need_no_slots();
    test_invariant_sweep();

    if (s_failures != 0)
        fprintf(stderr, "%d handler test(s) failed\n", s_failures);
    return s_failures == 0 ? 0 : 1;
}
