#!/bin/sh
set -eu

root="$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)"
out="${TMPDIR:-/tmp}/dragon-core-evlog-cursor-test"

cc -std=c11 -Wall -Wextra -Werror \
  -I"$root/tests/stubs" \
  -I"$root/components/dc_evlog/include" \
  "$root/tests/dc_evlog_cursor_host_test.c" \
  "$root/components/dc_evlog/dc_evlog.c" \
  -o "$out"

"$out"
