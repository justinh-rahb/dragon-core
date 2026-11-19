#!/usr/bin/env bash
set -euo pipefail

root="$(cd "$(dirname "$0")/.." && pwd)"
out="$(mktemp)"
trap 'rm -f "$out"' EXIT

# dc_prusa_parse.h pulls in cJSON; vendored under tests/vendor/cjson for the host
# build (the device build uses the ESP-IDF `json` component).
cc -std=c11 -Wall -Wextra -Werror -fsanitize=address,undefined \
  -I"$root/components/dc_prusa/include" \
  -I"$root/tests/vendor/cjson" \
  "$root/tests/dc_prusa_parse_host_test.c" \
  "$root/tests/vendor/cjson/cJSON.c" \
  -o "$out"
"$out"
