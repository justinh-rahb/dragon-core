#!/bin/sh
set -eu

root="$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)"
out="${TMPDIR:-/tmp}/dragon-core-dc-bambu-test"

# dc_bambu_parse.h is pure (only libc), so no ESP stubs are needed — just its
# include dir for the header under test.
cc -std=c11 -Wall -Wextra -Werror \
  -I"$root/components/dc_bambu/include" \
  "$root/tests/dc_bambu_host_test.c" \
  -o "$out"

"$out"
