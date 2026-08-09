#!/bin/sh
set -eu

root="$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)"
out="${TMPDIR:-/tmp}/dragon-core-dc-wifi-validate-test"

cc -std=c11 -Wall -Wextra -Werror \
  -I"$root/tests/stubs" \
  -I"$root/components/dc_wifi/include" \
  "$root/components/dc_wifi/dc_wifi_validate.c" \
  "$root/tests/dc_wifi_validate_host_test.c" \
  -o "$out"

"$out"
