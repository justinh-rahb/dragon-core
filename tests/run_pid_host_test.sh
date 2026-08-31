#!/bin/sh
set -eu
root="$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)"
out="${TMPDIR:-/tmp}/dragon-core-dc-pid-test"

cc -std=c11 -Wall -Wextra -Werror \
  -I"$root/components/dc_pid/include" \
  "$root/components/dc_pid/dc_pid.c" \
  "$root/tests/dc_pid_host_test.c" \
  -lm -o "$out"

"$out"
