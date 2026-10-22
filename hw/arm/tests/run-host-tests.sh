#!/bin/sh
#
# Build and run the host-side unit tests for hw/arm sources that need only the
# standard C headers (no QEMU build, no guest firmware).
#
#   hw/arm/tests/run-host-tests.sh [output dir]
#
# Environment: CC (default `cc`).
#
# SPDX-License-Identifier: AGPL-3.0-or-later

set -eu
ROOT=$(cd "$(dirname "$0")/../../.." && pwd)
OUT=${1:-${TMPDIR:-/tmp}}
CC=${CC:-cc}

mkdir -p "$OUT"
"$CC" -std=gnu11 -O1 -g -Wall -Wextra -Werror -fsanitize=undefined -fno-sanitize-recover=undefined \
    -I"$ROOT/hw/arm/tests/host-stub" -I"$ROOT/include" \
    -o "$OUT/sep_rom_idle_test" \
    "$ROOT/hw/arm/tests/sep_rom_idle_test.c" "$ROOT/hw/arm/sep/rom-idle.c"
"$OUT/sep_rom_idle_test"

"$CC" -std=gnu11 -O1 -g -Wall -Wextra -Werror -fsanitize=undefined -fno-sanitize-recover=undefined \
    -I"$ROOT/hw/arm/tests/host-stub" -I"$ROOT/include" \
    -o "$OUT/agx_uat_test" \
    "$ROOT/hw/arm/tests/agx_uat_test.c" "$ROOT/hw/gpu/apple-agx/agx-uat.c"
"$OUT/agx_uat_test"
