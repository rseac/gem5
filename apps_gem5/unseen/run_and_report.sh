#!/usr/bin/env bash
# Run the "unseen" benchmarks (see build.sh) against the gem5 AraCoprocessor
# and report MAPE against the matching Ara RTL/Verilator baseline (the
# 4L_4096V row of paper_results/plot_ara_unseen_heatmap.py's embedded CSV -
# num_lanes=4, vlen=4096, matching run_gem5_ara.py's AraCoprocessor config).
#
# Run from the gem5 repo root, after apps_gem5/unseen/build.sh:
#   apps_gem5/unseen/run_and_report.sh

set -uo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$REPO_ROOT"

# name -> RTL ROI cycles (4L_4096V row). dotproduct has no single canonical
# ROI measurement in its own source (it prints one "Vector runtime" per
# inner stripmining iteration across 4 datatypes, no outer wrap) - our
# build adds a whole-program [ROI-LATENCY] wrap as a best-effort stand-in,
# which is NOT the same region the RTL baseline measured, so its MAPE is
# not reported.
declare -A RTL_CYCLES=(
  [fconv2d]=156834
  [fconv3d]=479684
  [fmatmul]=532903
  [iconv2d]=155748
)
NOT_COMPARABLE="dotproduct"

echo "Benchmark    | gem5 cycles | RTL cycles | MAPE"
echo "-------------|-------------|------------|------"

for bench in dotproduct fconv2d fconv3d fmatmul iconv2d; do
  BIN="apps_gem5/bin/${bench}_se.exe"
  if [ ! -f "$BIN" ]; then
    echo "$bench: binary not found - run build.sh first" >&2
    continue
  fi

  LOG=$(mktemp)
  docker run --rm -u "$(id -u)":"$(id -g)" -v "$(pwd)":/gem5 -w /gem5 \
    rseac/gem5-ara:latest build/RISCV/gem5.opt run_gem5_ara.py "$BIN" \
    > "$LOG" 2>&1

  # AraCoprocessor's own conditional debug printf (ara_coprocessor.hh,
  # gated on register v7) can race with the guest program's own write()
  # and land mid-line (e.g. "[v7] remoThe execution took 1234 cycles.") -
  # match loosely so a merged line still parses.
  CYCLES=$(grep -oE '(execution took|ROI-LATENCY\]:) [0-9]+' "$LOG" | grep -oE '[0-9]+' | tail -1)
  rm -f "$LOG"

  if [ -z "${CYCLES:-}" ]; then
    printf "%-12s | %-11s | %-10s | %s\n" "$bench" "N/A" "-" "run produced no cycle count"
    continue
  fi

  if [ "$bench" = "$NOT_COMPARABLE" ]; then
    printf "%-12s | %-11s | %-10s | %s\n" "$bench" "$CYCLES" "686*" "not comparable, see script header"
    continue
  fi

  RTL=${RTL_CYCLES[$bench]}
  MAPE=$(python3 -c "print(f'{abs($CYCLES - $RTL) / $RTL * 100:.1f}%')")
  printf "%-12s | %-11s | %-10s | %s\n" "$bench" "$CYCLES" "$RTL" "$MAPE"
done
