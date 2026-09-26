#pragma once

// Private, transport-free half of dc_logtail: request parsing, authorization
// policy and the mapping from a dc_evlog cursor read to an HTTP response plan.
// Kept free of esp_http_server so the whole contract is host-testable; the
// handler in dc_logtail.c only emits what this layer decides.

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "dc_evlog.h"

// Longest query string accepted ("cursor=" + 20 digits fits comfortably).
#define DC_LOGTAIL_QUERY_MAX    64
// Decimal digits of UINT64_MAX plus the NUL.
#define DC_LOGTAIL_U64_CHARS    21
#define DC_LOGTAIL_MAX_HEADERS  8

#define DC_LOGTAIL_HDR_BOOT_ID    "Dragon-Log-Boot-Id"
#define DC_LOGTAIL_HDR_CURSOR     "Dragon-Log-Cursor"
#define DC_LOGTAIL_HDR_OLDEST     "Dragon-Log-Oldest-Seq"
#define DC_LOGTAIL_HDR_START      "Dragon-Log-Start-Seq"
#define DC_LOGTAIL_HDR_END        "Dragon-Log-End-Seq"
#define DC_LOGTAIL_HDR_WRITE      "Dragon-Log-Write-Seq"
#define DC_LOGTAIL_HDR_LOST       "Dragon-Log-Lost-Bytes"
#define DC_LOGTAIL_HDR_CAPACITY   "Dragon-Log-Capacity"

// Strict canonical unsigned decimal: 1..20 ASCII digits, no sign, no
// whitespace, no leading zero (except "0" itself), value <= UINT64_MAX.
bool dc_logtail_parse_u64(const char *s, size_t len, uint64_t *out);

// Parses a raw (undecoded) URL query string. Accepts exactly one "cursor=N"
// parameter and nothing else; duplicate, missing or unknown parameters and
// any percent-encoding are rejected.
bool dc_logtail_parse_query(const char *query, uint64_t *cursor);

// Writes the decimal form of v plus NUL; returns the digit count. Does not
// depend on printf's 64-bit support (newlib-nano lacks %llu).
size_t dc_logtail_format_u64(uint64_t v, char out[DC_LOGTAIL_U64_CHARS]);

// 1..DC_LOGTAIL_BOOT_ID_MAX characters from [A-Za-z0-9._:-]. The charset
// makes the value safe to emit verbatim as an HTTP header value.
bool dc_logtail_boot_id_valid(const char *boot_id);

typedef enum {
    DC_LOGTAIL_AUTH_INVALID = 0,  // neither or both set: refuse to register
    DC_LOGTAIL_AUTH_CALLBACK,
    DC_LOGTAIL_AUTH_OPEN,
} dc_logtail_auth_mode_t;

dc_logtail_auth_mode_t dc_logtail_auth_mode(bool has_callback,
                                            bool allow_unauthenticated);
// Default-deny: only OPEN, or CALLBACK with a true callback result, permits.
bool dc_logtail_auth_permits(dc_logtail_auth_mode_t mode, bool callback_result);

typedef struct {
    const char *name;
    const char *value;
} dc_logtail_header_t;

typedef struct {
    const char *status;        // HTTP status line text, e.g. "200 OK"
    const char *content_type;
    const char *body;          // error body, or NULL for the payload case
    size_t body_len;           // payload length when body == NULL
    size_t header_count;
    dc_logtail_header_t headers[DC_LOGTAIL_MAX_HEADERS];
    // Backing storage for numeric header values; header pointers refer into
    // this struct, so it must outlive the HTTP send.
    char num[DC_LOGTAIL_MAX_HEADERS][DC_LOGTAIL_U64_CHARS];
} dc_logtail_response_t;

typedef enum {
    DC_LOGTAIL_ERR_BAD_REQUEST,
    DC_LOGTAIL_ERR_FORBIDDEN,
} dc_logtail_error_t;

// Response for a completed dc_evlog_console_read(). `boot_id` must outlive
// the response. OK -> 200 with all eight metadata headers; FUTURE_CURSOR ->
// 409 with boot id, cursor, oldest, write and capacity; anything else -> 500.
void dc_logtail_build_read_response(dc_evlog_console_read_status_t status,
                                    const dc_evlog_console_read_t *info,
                                    const char *boot_id,
                                    dc_logtail_response_t *out);

// Response for a request refused before any ring access. Carries no metadata:
// a refused or malformed request learns nothing about the ring or the boot.
void dc_logtail_build_error_response(dc_logtail_error_t error,
                                     dc_logtail_response_t *out);
