#!/usr/bin/env bash
# Build the "unseen" benchmarks (dotproduct, fconv2d, fconv3d, fmatmul,
# iconv2d - the set plotted by paper_results/plot_ara_unseen_heatmap.py,
# excluding gemv) for gem5 SE mode.
#
# These live directly under apps_gem5/<name>/ (the native AraXL bare-metal
# benchmark tree, not the riscv-vectorized-benchmark-suite family used by
# the rest of apps_gem5/bin/*_se.exe). They already carry a compiled-in
# data.S (generated once via each benchmark's script/gen_data.py) rather
# than reading input from argv, so no run-time arguments are needed.
#
# Run from the gem5 repo root (this wraps docker itself):
#   apps_gem5/unseen/build.sh
#
# Key gotcha (see apps_gem5/se_build/README.md for the riscv-vectorized
# family's version of this): these benchmarks' own runtime.h ties the
# *timer* functions (start_timer/stop_timer/get_timer), not just printf,
# to the SPIKE macro - `-DSPIKE` silently makes get_timer() always return
# 0. Do NOT define -DSPIKE. Instead put se_build/shim first on the include
# path so `#include "printf.h"` (guarded by `#ifndef SPIKE`, which stays
# untrue) resolves to the shim's `#include <stdio.h>` instead of the
# bare-metal embedded printf implementation - this keeps the real,
# working timer intact while still getting a libc-backed printf that SE
# mode understands.

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$REPO_ROOT"

BENCHES="dotproduct fconv2d fconv3d fmatmul iconv2d"

docker run --rm -v "$(pwd)":/gem5 -w /gem5 rseac/gem5-ara:latest bash -c '
set -euo pipefail
CC=/riscv/_install/bin/riscv64-unknown-linux-gnu-gcc
SUITE=/gem5/apps_gem5
OUT_DIR="$SUITE/bin"
mkdir -p "$OUT_DIR"

IFLAGS_COMMON="-I $SUITE/se_build/shim -I $SUITE/common"
DEFS="-DNR_LANES=4 -DNR_CLUSTERS=1"
COMMON_SRCS="$SUITE/common/util.c $SUITE/se_build/shim/compat_globals.c"

for bench in '"$BENCHES"'; do
  echo "=== Building $bench ==="
  BDIR="$SUITE/$bench"
  SRCS=$(find "$BDIR" -maxdepth 2 -name "*.c" ! -name "main.c")
  $CC -march=rv64gcv $DEFS -static -O2 -fno-tree-vectorize \
    $IFLAGS_COMMON -I "$BDIR" \
    "$BDIR/main.c" $SRCS "$BDIR/data.S" $COMMON_SRCS \
    -o "$OUT_DIR/${bench}_se.exe" -lm
  echo "  -> $OUT_DIR/${bench}_se.exe"
done
'

echo "Done. Binaries in apps_gem5/bin/{$(echo $BENCHES | tr ' ' ',')}_se.exe"
