# dc_logtail

`dc_logtail` is a passive, read-only HTTP export of the raw ESP-IDF console byte
ring owned by `dc_evlog`. A client polls with an absolute byte cursor and receives
the exact captured bytes plus the metadata needed to prove what it received and
what it missed. It is a diagnostics/evidence facility only: no control channel,
no push transport, no per-client device state.

It exports only the console byte ring. The curated `dc_evlog_add()` event ring
(`GET /api/v1/system/logs` in `dc_portal`) and product structured events or
telemetry are separate and are not served here.

## Integration

The product owns the `esp_http_server` instance, the boot identity and the
authorization policy:

```c
static bool my_authorize(httpd_req_t *req, void *ctx);   // product policy

dc_logtail_config_t logtail = {
    .boot_id   = my_boot_id,    // same identity as the product's other surfaces
    .authorize = my_authorize,  // or .allow_unauthenticated = true, never both
    .auth_ctx  = my_ctx,
};
ESP_ERROR_CHECK(dc_logtail_register(server, &logtail));
```

With `dc_portal`, call `dc_logtail_register()` from `register_product_routes`, so
the route is installed before the portal's `/*` catch-all.

A zero-initialised config is rejected. A NULL `authorize` never means public
access; unauthenticated export requires `allow_unauthenticated = true`. The first
successful registration latches the configuration for the boot; re-registering
after an HTTP server restart must pass an identical configuration.

## Request

```
GET /api/v1/system/log?cursor=N
```

`N` is an absolute byte sequence number for the current boot, written as
canonical unsigned decimal (`0`, or 1–20 digits with no leading zero, at most
`18446744073709551615`). Exactly one `cursor` parameter is accepted; a missing,
empty, signed, fractional, overflowing, percent-encoded, whitespace-padded or
duplicated cursor, or any other query parameter, is a `400`. There is no `max`
parameter: the payload cap is fixed (see below).

## Cursor semantics

With `W` = the current write sequence and `C` = ring capacity, the retained
window is `[oldest, W)` where `oldest = max(0, W - C)`.

| Requested cursor       | Result                                                     |
| ---------------------- | ---------------------------------------------------------- |
| `oldest <= cursor <= W` | `200`; starts exactly at `cursor`; `lost_bytes = 0`        |
| `cursor < oldest`       | `200`; starts at `oldest`; `lost_bytes = oldest - cursor`  |
| `cursor > W`            | `409`; nothing returned; the cursor is never rewound       |

All ranges are half-open. For every `200`:

```
payload bytes == raw ring bytes in [start_seq, end_seq)
Content-Length == end_seq - start_seq
next_cursor    == end_seq
```

`cursor == W` is a successful empty read: `200`, zero-length body, complete
metadata, next cursor unchanged. `200` rather than `204` keeps one success code
and one metadata-handling path for every valid read. When `end_seq < write_seq`
more data is already available and the client may poll again immediately.

Loss is exact raw-byte loss. There are no line counts, per-line sequence numbers
or per-line timestamps; the ESP-IDF textual log prefix remains ordinary text in
the payload.

## Response

`200` bodies are `Content-Type: application/octet-stream` and are byte-exact:
no UTF-8 sanitising or replacement, no line-ending normalisation, no prefix
parsing, no JSON encoding. Any decoded presentation belongs host-side, with the
raw bytes retained.

| Header                  | `200` | `409` | Meaning                                  |
| ----------------------- | :---: | :---: | ---------------------------------------- |
| `Dragon-Log-Boot-Id`    |   ✓   |   ✓   | product-supplied boot/session identity   |
| `Dragon-Log-Cursor`     |   ✓   |   ✓   | requested cursor, echoed                 |
| `Dragon-Log-Oldest-Seq` |   ✓   |   ✓   | oldest retained byte at the read         |
| `Dragon-Log-Start-Seq`  |   ✓   |       | first returned byte                      |
| `Dragon-Log-End-Seq`    |   ✓   |       | one past the last returned byte          |
| `Dragon-Log-Write-Seq`  |   ✓   |   ✓   | producer position observed by the read   |
| `Dragon-Log-Lost-Bytes` |   ✓   |       | `start_seq - cursor`                     |
| `Dragon-Log-Capacity`   |   ✓   |   ✓   | console ring capacity in bytes           |

All sequence values are unsigned 64-bit decimal. Every `200` value is taken from
the same atomic read as the payload.

| Status | When                                                        | Body |
| ------ | ----------------------------------------------------------- | ---- |
| `200`  | valid cursor                                                | raw bytes |
| `400`  | malformed/missing cursor or unexpected parameter            | `{"ok":false,"error":"invalid cursor"}` |
| `403`  | product authorization refused                               | `{"ok":false,"error":"authorization required"}` |
| `409`  | `cursor > write_seq`                                        | `{"ok":false,"error":"cursor ahead of write_seq"}` |
| `500`  | internal failure, including a server with too few response-header slots | JSON error |

`400` and `403` carry no ring or boot metadata. `403` follows the `dc_portal`
convention. `409` follows the family convention for a well-formed request that
conflicts with current device state; its metadata lets a client resynchronise
without a second request. Clients must ignore `Dragon-Log-*` headers on any
status other than `200` and `409`.

The eight `200` headers exactly fill ESP-IDF's default `max_resp_headers` (8).
`Cache-Control: no-store` is added only when the server has a spare slot.

## Reboot semantics

Sequence numbers restart at 0 on every boot and the product's boot identity
changes. A consumer must store the boot identity alongside its cursor and compare
it with `Dragon-Log-Boot-Id` on every response. A saved cursor from a previous
boot can be numerically valid in the new boot; on a boot-identity mismatch the
consumer discards that response and restarts from `cursor=0` (or whatever its
ingestion contract specifies). Core keeps no persistent state to detect this.

## Payload cap, locking and backpressure

Each response carries at most `DC_LOGTAIL_MAX_PAYLOAD` (1024) bytes. The bytes are
copied from the ring in one short spinlock critical section into a buffer on the
httpd task stack, and the lock is released before any header formatting or
socket I/O. 1 KiB matches the existing `/console` streaming chunk on the same
task and bounds lock hold time to a 1 KiB `memcpy`. Measured with
`-fstack-usage` (ESP-IDF v5.3, esp32c3, default `-Og`), the handler frame is
1424 bytes plus 288 for the response emitter, about 1.7 KiB before
esp_http_server's own send path. That fits `dc_portal`'s 8 KiB floor
comfortably; products running their own server on ESP-IDF's 4 KiB default
stack should budget for it or raise the stack. Draining a full 16 KiB ring
takes 16 back-to-back requests.

There is no exporter queue, no per-client state and no heap allocation. The
logging producer path is unchanged and never touches the network, so slow, dead
or absent clients cannot stall or slow console producers. A client that falls
behind simply observes `lost_bytes`.

## Logging and security

The request path never calls `ESP_LOGx`: the exported ring is fed by `ESP_LOGx`,
so logging a poll would manufacture evidence. Rejections are sent without
`httpd_resp_send_err()`, which logs. Two caveats remain outside this component:
esp_http_server itself logs genuine socket failures, and a product `authorize`
callback must likewise not log on routine paths. Raising the esp_http_server log
level to DEBUG makes it log every request, which would appear in the exported
ring.

The exporter never echoes request headers, Authorization values or credentials,
and defines no credentials of its own. It does not redact log text: the console
export exposes whatever the firmware logs, so keeping secrets out of logs remains
a source-level requirement.
