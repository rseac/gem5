#!/usr/bin/env bash
# Sweep all 9 Lane/VLEN configs VP++ was validated against (2/4/8 lanes x
# 1024/2048/4096 VLEN - paper_results/plot_ara_unseen_heatmap.py) for all
# 5 "unseen" benchmarks, and report gem5 AraCoprocessor's MAPE against the
# same RTL baseline alongside VP++'s own MAPE, for direct comparison.
#
# Requires apps_gem5/unseen/sweep_build.sh to have been run first (needs
# the per-lane-count *_se_{2,4,8}L.exe binaries, not the single default
# *_se.exe built by build.sh).
#
# Runs 3 gem5 invocations concurrently (each config/benchmark pair is
# fully independent - separate docker containers, separate binaries).
#
# Run from the gem5 repo root:
#   apps_gem5/unseen/sweep_run.sh

set -uo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$REPO_ROOT"

# RTL_<config>_<bench> and VP_<config>_<bench>, from the embedded CSV in
# paper_results/plot_ara_unseen_heatmap.py (gemv excluded there too).
declare -A RTL VP
read -r -d '' TABLE <<'EOF'
2L_1024V dotproduct 1215 1181
2L_1024V fconv2d 320535 329553
2L_1024V fconv3d 982587 998198
2L_1024V fmatmul 1106743 1069043
2L_1024V iconv2d 316698 333655
2L_2048V dotproduct 1216 1199
2L_2048V fconv2d 313857 321867
2L_2048V fconv3d 958384 974128
2L_2048V fmatmul 1065178 1066829
2L_2048V iconv2d 310049 323781
2L_4096V dotproduct 1259 1303
2L_4096V fconv2d 316343 319172
2L_4096V fconv3d 954195 963819
2L_4096V fmatmul 1062879 1065946
2L_4096V iconv2d 309068 320130
4L_1024V dotproduct 693 637
4L_1024V fconv2d 179549 176885
4L_1024V fconv3d 543899 548006
4L_1024V fmatmul 739757 557747
4L_1024V iconv2d 176851 181910
4L_2048V dotproduct 676 623
4L_2048V fconv2d 160184 165019
4L_2048V fconv3d 491891 497626
4L_2048V fmatmul 555657 534573
4L_2048V iconv2d 159111 166937
4L_4096V dotproduct 686 663
4L_4096V fconv2d 156834 160971
4L_4096V fconv3d 479684 487105
4L_4096V fmatmul 532903 533466
4L_4096V iconv2d 155748 161929
8L_1024V dotproduct 431 375
8L_1024V fconv2d 137265 117286
8L_1024V fconv3d 395973 378915
8L_1024V fmatmul 629288 312307
8L_1024V iconv2d 135773 122101
8L_2048V dotproduct 402 345
8L_2048V fconv2d 89405 87112
8L_2048V fconv3d 269815 271051
8L_2048V fmatmul 363317 278925
8L_2048V iconv2d 92819 89836
8L_4096V dotproduct 397 353
8L_4096V fconv2d 80189 82544
8L_4096V fconv3d 249231 248735
8L_4096V fmatmul 277426 267338
8L_4096V iconv2d 85916 83503
EOF

while read -r config bench rtl vp; do
  RTL["${config}_${bench}"]=$rtl
  VP["${config}_${bench}"]=$vp
done <<< "$TABLE"

RESULTS_DIR=$(mktemp -d)
export RESULTS_DIR REPO_ROOT

run_one() {
  local LANES=$1 VLEN=$2 bench=$3
  local CONFIG="${LANES}L_${VLEN}V"
  local BIN="apps_gem5/bin/${bench}_se_${LANES}L.exe"
  local OUT="$RESULTS_DIR/${CONFIG}_${bench}"

  if [ ! -f "$BIN" ]; then
    echo "$CONFIG $bench NOBIN" > "$OUT"
    return
  fi

  local LOG
  LOG=$(mktemp)
  docker run --rm -u "$(id -u)":"$(id -g)" \
    -e ARA_LANES="$LANES" -e ARA_VLEN="$VLEN" \
    -v "$REPO_ROOT":/gem5 -w /gem5 \
    rseac/gem5-ara:latest build/RISCV/gem5.opt run_gem5_ara.py "$BIN" \
    > "$LOG" 2>&1

  local CYCLES
  CYCLES=$(grep -oE '(execution took|ROI-LATENCY\]:) [0-9]+' "$LOG" | grep -oE '[0-9]+' | tail -1)
  rm -f "$LOG"

  if [ -z "${CYCLES:-}" ]; then
    echo "$CONFIG $bench NOCYCLES" > "$OUT"
  else
    echo "$CONFIG $bench $CYCLES" > "$OUT"
  fi
}
export -f run_one

JOBS=()
for LANES in 2 4 8; do
  for VLEN in 1024 2048 4096; do
    for bench in dotproduct fconv2d fconv3d fmatmul iconv2d; do
      JOBS+=("$LANES $VLEN $bench")
    done
  done
done

printf '%s\n' "${JOBS[@]}" | xargs -P 3 -L 1 bash -c 'run_one $0 $1 $2'

printf "%-10s %-11s | %-11s | %-9s %-9s | %-9s %-9s\n" \
  "Config" "Benchmark" "gem5" "RTL" "VP++" "gem5MAPE" "VP++MAPE"
printf "%-10s %-11s | %-11s | %-9s %-9s | %-9s %-9s\n" \
  "------" "---------" "----" "---" "----" "--------" "--------"

for LANES in 2 4 8; do
  for VLEN in 1024 2048 4096; do
    CONFIG="${LANES}L_${VLEN}V"
    for bench in dotproduct fconv2d fconv3d fmatmul iconv2d; do
      KEY="${CONFIG}_${bench}"
      RES_FILE="$RESULTS_DIR/${KEY}"
      RTL_C=${RTL[$KEY]:-}
      VP_C=${VP[$KEY]:-}

      if [ ! -f "$RES_FILE" ]; then
        printf "%-10s %-11s | %s\n" "$CONFIG" "$bench" "no result file"
        continue
      fi
      CYCLES=$(awk '{print $3}' "$RES_FILE")

      if [ "$CYCLES" = "NOBIN" ] || [ "$CYCLES" = "NOCYCLES" ]; then
        printf "%-10s %-11s | %s\n" "$CONFIG" "$bench" "$CYCLES"
        continue
      fi

      GEM5_MAPE=$(python3 -c "print(f'{abs($CYCLES - $RTL_C) / $RTL_C * 100:.1f}%')")
      VP_MAPE=$(python3 -c "print(f'{abs($VP_C - $RTL_C) / $RTL_C * 100:.1f}%')")
      printf "%-10s %-11s | %-11s | %-9s %-9s | %-9s %-9s\n" \
        "$CONFIG" "$bench" "$CYCLES" "$RTL_C" "$VP_C" "$GEM5_MAPE" "$VP_MAPE"
    done
  done
done

rm -rf "$RESULTS_DIR"
