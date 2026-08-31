#!/bin/sh
set -eu

root="$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)"
out="${TMPDIR:-/tmp}/dragon-core-console-stream-test"

cc -std=c11 -Wall -Wextra -Werror \
  -I"$root/tests/stubs" \
  -I"$root/components/dc_evlog/include" \
  -I"$root/components/dc_portal" \
  "$root/tests/dc_console_stream_host_test.c" \
  "$root/components/dc_evlog/dc_evlog.c" \
  "$root/components/dc_portal/dc_portal_console_stream.c" \
  -o "$out"

"$out"
