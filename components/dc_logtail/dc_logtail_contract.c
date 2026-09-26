#include "dc_logtail_contract.h"

#include "dc_logtail_limits.h"

#include <string.h>

static size_t bounded_len(const char *s, size_t limit)
{
    size_t n = 0;
    while (n <= limit && s[n] != '\0') n++;
    return n;   // > limit means "too long"
}

static const char BODY_BAD_REQUEST[] = "{\"ok\":false,\"error\":\"invalid cursor\"}";
static const char BODY_FORBIDDEN[] = "{\"ok\":false,\"error\":\"authorization required\"}";
static const char BODY_FUTURE[] = "{\"ok\":false,\"error\":\"cursor ahead of write_seq\"}";
static const char BODY_INTERNAL[] = "{\"ok\":false,\"error\":\"console read failed\"}";

bool dc_logtail_parse_u64(const char *s, size_t len, uint64_t *out)
{
    if (s == NULL || out == NULL || len == 0 || len > DC_LOGTAIL_U64_CHARS - 1)
        return false;
    if (len > 1 && s[0] == '0') return false;

    uint64_t v = 0;
    for (size_t i = 0; i < len; ++i) {
        if (s[i] < '0' || s[i] > '9') return false;
        uint64_t d = (uint64_t)(s[i] - '0');
        if (v > (UINT64_MAX - d) / 10u) return false;
        v = v * 10u + d;
    }
    *out = v;
    return true;
}

bool dc_logtail_parse_query(const char *query, uint64_t *cursor)
{
    static const char KEY[] = "cursor=";
    const size_t key_len = sizeof(KEY) - 1;
    if (query == NULL || cursor == NULL) return false;

    // Exactly one parameter. The query must begin with "cursor=" and the rest
    // must be strict digits, so any separator ('&', ';'), duplicate, unknown
    // parameter or percent-encoding fails the prefix or the digit parse.
    size_t len = bounded_len(query, DC_LOGTAIL_QUERY_MAX);
    if (len > DC_LOGTAIL_QUERY_MAX) return false;
    if (len <= key_len || memcmp(query, KEY, key_len) != 0) return false;
    return dc_logtail_parse_u64(query + key_len, len - key_len, cursor);
}

size_t dc_logtail_format_u64(uint64_t v, char out[DC_LOGTAIL_U64_CHARS])
{
    char tmp[DC_LOGTAIL_U64_CHARS];
    size_t n = 0;
    do {
        tmp[n++] = (char)('0' + (v % 10u));
        v /= 10u;
    } while (v != 0);
    for (size_t i = 0; i < n; ++i) out[i] = tmp[n - 1 - i];
    out[n] = '\0';
    return n;
}

bool dc_logtail_boot_id_valid(const char *boot_id)
{
    if (boot_id == NULL) return false;
    size_t len = bounded_len(boot_id, DC_LOGTAIL_BOOT_ID_MAX);
    if (len == 0 || len > DC_LOGTAIL_BOOT_ID_MAX) return false;
    for (size_t i = 0; i < len; ++i) {
        char c = boot_id[i];
        bool ok = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                  (c >= '0' && c <= '9') || c == '.' || c == '_' ||
                  c == ':' || c == '-';
        if (!ok) return false;
    }
    return true;
}

dc_logtail_auth_mode_t dc_logtail_auth_mode(bool has_callback,
                                            bool allow_unauthenticated)
{
    if (has_callback == allow_unauthenticated) return DC_LOGTAIL_AUTH_INVALID;
    return has_callback ? DC_LOGTAIL_AUTH_CALLBACK : DC_LOGTAIL_AUTH_OPEN;
}

bool dc_logtail_auth_permits(dc_logtail_auth_mode_t mode, bool callback_result)
{
    switch (mode) {
    case DC_LOGTAIL_AUTH_OPEN: return true;
    case DC_LOGTAIL_AUTH_CALLBACK: return callback_result;
    default: return false;
    }
}

static void reset(dc_logtail_response_t *out)
{
    memset(out, 0, sizeof(*out));
}

static void set_error_body(dc_logtail_response_t *out, const char *status,
                           const char *body, size_t body_len)
{
    out->status = status;
    out->content_type = "application/json";
    out->body = body;
    out->body_len = body_len;
}

static void add_text(dc_logtail_response_t *out, const char *name, const char *value)
{
    dc_logtail_header_t *h = &out->headers[out->header_count++];
    h->name = name;
    h->value = value;
}

static void add_u64(dc_logtail_response_t *out, const char *name, uint64_t v)
{
    char *slot = out->num[out->header_count];
    dc_logtail_format_u64(v, slot);
    add_text(out, name, slot);
}

void dc_logtail_build_read_response(dc_evlog_console_read_status_t status,
                                    const dc_evlog_console_read_t *info,
                                    const char *boot_id,
                                    dc_logtail_response_t *out)
{
    reset(out);
    if (info == NULL || boot_id == NULL) status = DC_EVLOG_CONSOLE_READ_INVALID_ARG;

    switch (status) {
    case DC_EVLOG_CONSOLE_READ_OK:
        out->status = "200 OK";
        out->content_type = "application/octet-stream";
        out->body = NULL;
        out->body_len = info->len;
        add_text(out, DC_LOGTAIL_HDR_BOOT_ID, boot_id);
        add_u64(out, DC_LOGTAIL_HDR_CURSOR, info->cursor);
        add_u64(out, DC_LOGTAIL_HDR_OLDEST, info->oldest_seq);
        add_u64(out, DC_LOGTAIL_HDR_START, info->start_seq);
        add_u64(out, DC_LOGTAIL_HDR_END, info->end_seq);
        add_u64(out, DC_LOGTAIL_HDR_WRITE, info->write_seq);
        add_u64(out, DC_LOGTAIL_HDR_LOST, info->lost_bytes);
        add_u64(out, DC_LOGTAIL_HDR_CAPACITY, (uint64_t)info->capacity);
        return;
    case DC_EVLOG_CONSOLE_READ_FUTURE_CURSOR:
        set_error_body(out, "409 Conflict", BODY_FUTURE, sizeof(BODY_FUTURE) - 1);
        add_text(out, DC_LOGTAIL_HDR_BOOT_ID, boot_id);
        add_u64(out, DC_LOGTAIL_HDR_CURSOR, info->cursor);
        add_u64(out, DC_LOGTAIL_HDR_OLDEST, info->oldest_seq);
        add_u64(out, DC_LOGTAIL_HDR_WRITE, info->write_seq);
        add_u64(out, DC_LOGTAIL_HDR_CAPACITY, (uint64_t)info->capacity);
        return;
    default:
        set_error_body(out, "500 Internal Server Error", BODY_INTERNAL,
                       sizeof(BODY_INTERNAL) - 1);
        return;
    }
}

void dc_logtail_build_error_response(dc_logtail_error_t error,
                                     dc_logtail_response_t *out)
{
    reset(out);
    if (error == DC_LOGTAIL_ERR_FORBIDDEN)
        set_error_body(out, "403 Forbidden", BODY_FORBIDDEN, sizeof(BODY_FORBIDDEN) - 1);
    else
        set_error_body(out, "400 Bad Request", BODY_BAD_REQUEST,
                       sizeof(BODY_BAD_REQUEST) - 1);
}
