#!/usr/bin/env bash
# Build the 5 "unseen" benchmarks for all 3 lane counts VP++ was swept
# across (2, 4, 8 - see paper_results/plot_ara_unseen_heatmap.py). VLEN
# does NOT require a rebuild: it's purely a gem5-side simulation
# parameter (RiscvISA.vlen / AraCoprocessor.vlen, see run_gem5_ara.py's
# ARA_LANES/ARA_VLEN env vars), not something these benchmarks' own C
# sources or data.S depend on. Lane count DOES require a rebuild: data.S's
# `.balign NR_LANES*4*NR_CLUSTERS` alignment directives are resolved by
# the assembler from the -DNR_LANES compile flag, so a binary built for
# one lane count has data laid out assuming that lane count's alignment.
#
# Produces apps_gem5/bin/<bench>_se_<lanes>L.exe for lanes in {2,4,8}.
#
# Run from the gem5 repo root (wraps docker itself):
#   apps_gem5/unseen/sweep_build.sh

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$REPO_ROOT"

for LANES in 2 4 8; do
docker run --rm -v "$(pwd)":/gem5 -w /gem5 rseac/gem5-ara:latest bash -c '
set -euo pipefail
CC=/riscv/_install/bin/riscv64-unknown-linux-gnu-gcc
SUITE=/gem5/apps_gem5
OUT_DIR="$SUITE/bin"
mkdir -p "$OUT_DIR"
LANES='"$LANES"'

IFLAGS_COMMON="-I $SUITE/se_build/shim -I $SUITE/common"
DEFS="-DNR_LANES=$LANES -DNR_CLUSTERS=1"
COMMON_SRCS="$SUITE/common/util.c $SUITE/se_build/shim/compat_globals.c"

echo "--- generating data.S for NR_LANES=$LANES ---"
python3 "$SUITE/dotproduct/script/gen_data.py" 64        > "$SUITE/dotproduct/data.S"
python3 "$SUITE/fconv2d/script/gen_data.py"    112 112 7 > "$SUITE/fconv2d/data.S"
python3 "$SUITE/fconv3d/script/gen_data.py"    112 7     > "$SUITE/fconv3d/data.S"
python3 "$SUITE/fmatmul/script/gen_data.py"    128 128 128 > "$SUITE/fmatmul/data.S"
python3 "$SUITE/iconv2d/script/gen_data.py"    112 7     > "$SUITE/iconv2d/data.S"

for bench in dotproduct fconv2d fconv3d fmatmul iconv2d; do
  echo "=== Building $bench for NR_LANES=$LANES ==="
  BDIR="$SUITE/$bench"
  SRCS=$(find "$BDIR" -maxdepth 2 -name "*.c" ! -name "main.c")
  $CC -march=rv64gcv $DEFS -static -O2 -fno-tree-vectorize \
    $IFLAGS_COMMON -I "$BDIR" \
    "$BDIR/main.c" $SRCS "$BDIR/data.S" $COMMON_SRCS \
    -o "$OUT_DIR/${bench}_se_${LANES}L.exe" -lm
  echo "  -> $OUT_DIR/${bench}_se_${LANES}L.exe"
done
'
done

echo "Done. Binaries in apps_gem5/bin/{dotproduct,fconv2d,fconv3d,fmatmul,iconv2d}_se_{2,4,8}L.exe"
