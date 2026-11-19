#!/bin/sh
set -eu

root="$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)"
out="${TMPDIR:-/tmp}/dragon-core-console-stream-test"

cc -std=c11 -Wall -Wextra -Werror -DDC_CONSOLE_STREAM_COUNT_CRITICAL \
  -I"$root/tests/stubs" \
  -I"$root/components/dc_evlog/include" \
  -I"$root/components/dc_portal" \
  "$root/tests/dc_console_stream_host_test.c" \
  "$root/components/dc_evlog/dc_evlog.c" \
  "$root/components/dc_portal/dc_portal_console_stream.c" \
  -o "$out"

"$out"

# Negative control: compile a temporary copy with the former split-append
# behavior. The normal byte-content tests pass, but atomicity must fail.
scratch="$(mktemp -d)"
trap 'rm -rf "$scratch"' EXIT HUP INT TERM
python3 - "$root/components/dc_evlog/dc_evlog.c" "$scratch/dc_evlog.c" <<'PY'
from pathlib import Path
import sys
source = Path(sys.argv[1]).read_text()
single = "        con_append(line, (int)sizeof line - 1);"
split = ("        con_append(line, (int)prefix_len);\n"
         "        con_append(TRUNCATION_MARKER, (int)TRUNCATION_MARKER_LEN);")
if source.count(single) != 1:
    raise SystemExit("mutation anchor changed; update negative control")
Path(sys.argv[2]).write_text(source.replace(single, split))
PY
cc -std=c11 -Wall -Wextra -Werror -DDC_CONSOLE_STREAM_COUNT_CRITICAL \
  -I"$root/tests/stubs" \
  -I"$root/components/dc_evlog/include" \
  -I"$root/components/dc_portal" \
  "$root/tests/dc_console_stream_host_test.c" \
  "$scratch/dc_evlog.c" \
  "$root/components/dc_portal/dc_portal_console_stream.c" \
  -o "$scratch/console-stream-mutant"

if "$scratch/console-stream-mutant" > "$scratch/mutant.log" 2>&1; then
  echo "[FAIL] split-append mutation unexpectedly passed" >&2
  exit 1
fi
if ! grep -Fq "[FAIL] truncated entry appended in one critical section" "$scratch/mutant.log"; then
  echo "[FAIL] split-append mutation failed for an unexpected reason" >&2
  cat "$scratch/mutant.log" >&2
  exit 1
fi
echo "[PASS] split-append mutation rejected by critical-section regression"
