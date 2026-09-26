#!/bin/sh
set -eu

root="$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)"
out="${TMPDIR:-/tmp}/dragon-core-logtail-test"
fail=0

# Static contract checks. The exporter must never log (console output feeds the
# exported ring), and dc_evlog must stay network-free and independent of it.
strip_comments() { sed -e 's://.*$::' "$@"; }

if strip_comments "$root"/components/dc_logtail/*.c |
    grep -nE 'ESP_LOG|esp_log_write|ESP_EARLY_LOG|httpd_resp_send_err|printf'; then
  echo "[FAIL] dc_logtail sources must not log or print" >&2
  fail=1
else
  echo "[PASS] dc_logtail sources contain no logging calls"
fi
if grep -nE '#include *"(esp_http_server|esp_netif|dc_logtail)|#include *"lwip/' \
    "$root"/components/dc_evlog/*.c "$root"/components/dc_evlog/include/*.h ||
   grep -nE 'esp_http_server|esp_netif|lwip|dc_logtail' \
    "$root/components/dc_evlog/CMakeLists.txt"; then
  echo "[FAIL] dc_evlog must not depend on HTTP, network or dc_logtail" >&2
  fail=1
else
  echo "[PASS] dc_evlog has no HTTP/network/dc_logtail dependency"
fi

cc -std=c11 -Wall -Wextra -Werror \
  -I"$root/tests/stubs" \
  -I"$root/components/dc_evlog/include" \
  -I"$root/components/dc_logtail" \
  -I"$root/components/dc_logtail/include" \
  "$root/tests/dc_logtail_host_test.c" \
  "$root/components/dc_logtail/dc_logtail_contract.c" \
  "$root/components/dc_evlog/dc_evlog.c" \
  -o "$out"

"$out" || fail=1

# The real request handler against a minimal esp_http_server fake that keeps
# ESP-IDF's response-header slot semantics (tests/stubs_httpd).
handler_out="${TMPDIR:-/tmp}/dragon-core-logtail-handler-test"
cc -std=c11 -Wall -Wextra -Werror \
  -I"$root/tests/stubs_httpd" \
  -I"$root/tests/stubs" \
  -I"$root/components/dc_evlog/include" \
  -I"$root/components/dc_logtail" \
  -I"$root/components/dc_logtail/include" \
  "$root/tests/dc_logtail_handler_host_test.c" \
  "$root/components/dc_logtail/dc_logtail.c" \
  "$root/components/dc_logtail/dc_logtail_contract.c" \
  "$root/components/dc_evlog/dc_evlog.c" \
  -o "$handler_out"

"$handler_out" || fail=1
exit "$fail"
