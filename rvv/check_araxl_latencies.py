#!/usr/bin/env python3
"""
check_araxl_latencies.py
========================
Runs rvv_ara_latency_test.bin under gem5 with AraXL timing parameters and
verifies measured latencies match expected values.

Two test suites:

1. Standard AraXL config (NrLanes=4, NrClusters=2, VLEN=1024)
   ----------------------------------------------------------
   All short-pipeline operations are dominated by DISPATCH_FLOOR=6 regardless
   of cluster count (throughput doubles but microVl also doubles → same
   throughput_cycles).  Expected values are identical to the ARA checker.

2. Throughput differentiation test (NrLanes=1, VLEN=512)
   -------------------------------------------------------
   With only 1 lane, vadd_ew32 LMUL=m8 has:
     microVl=16, epc=NrClusters×1×2
     ARA  (NrClusters=1): tc=8, dynamicOpLatency=max(8,6)=8 → measured ~10
     AraXL(NrClusters=2): tc=4, dynamicOpLatency=max(4,6)=6 → measured ~8
   This verifies that NrClusters correctly scales the throughput in the model.

Expected values (standard config, VLEN=1024, NrLanes=4, NrClusters=2,
                 chainingLatency = max(pipe+2, DISPATCH_FLOOR=6)):

  All latency constants are identical between ARA and AraXL (EW8 FP
  difference is invisible at these widths — both hit DISPATCH_FLOOR=6).

  Short-pipeline ALU/Mul/FP ops:  chainingLatency=6, measured ~8 (+ O3 overhead)
  vfadd_ew64:                     chainingLatency=7, measured ~8
  vdiv_ew32 chain (vdiv+vadd):    18+6=24 per iter, measured ~23
  vdiv_ew64 chain (vdiv+vadd):    34+6=40 per iter, measured ~39
  vfdiv_ew32 / vfsqrt_ew32:       8 micro-ops × dynamicOpLatency(6) = 48

Usage
-----
  python3 gem5/rvv/check_araxl_latencies.py \\
      --gem5   build/RISCV/gem5.opt \\
      --script gem5/rvv/riscv-rvv-se-ara.py \\
      --binary gem5/rvv/rvv_ara_latency_test.bin \\
      [--vlen 1024] [--lanes 4] [--clusters 2] [--tol 1.5]

Exit code 0 if all checks pass, non-zero otherwise.
"""

import argparse
import re
import subprocess
import sys

# ---------------------------------------------------------------------------
# Expected latencies for AraXL standard config (VLEN=1024, NrLanes=4, NrClusters=2)
#
# Identical to ARA expected values because:
#   - dynamicOpLatency throughput_cycles = ceil(microVl / (NrClusters×NrLanes×(ELEN/SEW)))
#   - VLEN doubles (1024 vs 512) → microVl doubles (32 vs 16 per micro-op)
#   - NrClusters doubles → elements_per_cycle doubles (16 vs 8 for EW32, 4L)
#   - Ratio unchanged → same throughput_cycles → same dynamicOpLatency
#   All DISPATCH_FLOOR-clamped ops stay at 6 cycles chaining latency.
# ---------------------------------------------------------------------------
EXPECTED_STANDARD = {
    # name           raw_expected   ara_rtl  note
    "vadd_ew32":     (8,            6,       "ALU EW32 chainingLatency=6 + O3 overhead"),
    "vmul_ew32":     (8,            6,       "Mul EW32 chainingLatency=6 + O3 overhead"),
    "vdiv_ew32":     (24,           18,      "vdiv(18) + vadd(dynamicOpLatency=6) per iter"),
    "vdiv_ew64":     (40,           34,      "vdiv(34) + vadd(dynamicOpLatency=6) per iter"),
    "vfadd_ew32":    (8,            6,       "FPComp EW32 chainingLatency=6 + O3 overhead"),
    "vfadd_ew64":    (8,            7,       "FPComp EW64 chainingLatency=7 + O3 overhead"),
    "vfmul_ew32":    (8,            6,       "FPComp EW32 chainingLatency=6 + O3 overhead"),
    "vfmacc_ew32":   (8,            6,       "FPComp EW32 chainingLatency=6 + O3 overhead"),
    "vfmin_ew32":    (8,            6,       "FPNonComp EW32 chainingLatency=6 + O3 overhead"),
    "vfmin_ew64":    (8,            6,       "FPNonComp EW64 LatFNonComp=1; chainingLatency=max(3,6)=6 + O3 overhead"),
    "vfredusum_ew64":(12,           7,       "FP reduce-sum EW64: full reduction occupancy (all vl elements must complete); RTL LatFCompEW64=5"),
    "vfdiv_ew32":    (48,           6,       "FPDivSqrt EW32: 8 micro-ops × dynamicOpLatency(6) pipelined=False"),
    "vfsqrt_ew32":   (48,           6,       "FPDivSqrt EW32: 8 micro-ops × dynamicOpLatency(6) pipelined=False"),
    "vfcvt_ew32":    (8,            6,       "FPConv EW32 chainingLatency=6 + O3 overhead (per-op avg)"),
}

# ---------------------------------------------------------------------------
# Throughput differentiation test (NrLanes=1, VLEN=512)
#
# Pipelined ops (vadd, vfadd, etc.) use AraSIMD_Pipelined(count=2).  The dual
# FU slots let the O3 CPU absorb dynamicOpLatency > chainingLatency, so the
# measured chain spacing stays at ~chainingLatency regardless of lane count.
# These ops are NOT valid differentiators for NrClusters.
#
# Non-pipelined ops (vfdiv, vfsqrt) use AraSIMD_FPDivSqrt(count=1).
# With count=1 the FU serialises every micro-op, so the chain spacing IS
# determined by dynamicOpLatency:
#
#   vfdiv_ew32 LMUL=m8, VLEN=512, EW32: microVl=16, pipeline_lat=3
#     ARA  (1L/1C): epc=2,  tc=8  → dynamicOpLatency=max(11,6)=11 → 8×11=88
#     AraXL(1L/2C): epc=4,  tc=4  → dynamicOpLatency=max(7, 6)=7  → 8×7 =56
# ---------------------------------------------------------------------------
EXPECTED_THROUGHPUT_ARA   = {"vfdiv_ew32": (88, "ARA  1L/1C: 8 uops × dynamicOpLatency(11) = 88")}
EXPECTED_THROUGHPUT_ARAXL = {"vfdiv_ew32": (56, "AraXL 1L/2C: 8 uops × dynamicOpLatency(7)  = 56")}


# ---------------------------------------------------------------------------
# helpers
# ---------------------------------------------------------------------------

def run_gem5(gem5_bin, script, binary, vlen, lanes, clusters,
             model="ara", ring_latency=0, extra_args=None):
    """Run gem5 and return (returncode, combined_output)."""
    cmd = [
        gem5_bin,
        script,
        "--enable-chaining",
        "--vlen", str(vlen),
        "--vector-timing-throughput", str(lanes),
        "--vector-timing-model", model,
        "--nr-clusters", str(clusters),
        "--ring-latency", str(ring_latency),
        binary,
    ]
    if extra_args:
        cmd.extend(extra_args)
    print(f"\n[run] {' '.join(cmd)}")
    result = subprocess.run(cmd, capture_output=True, text=True, timeout=900)
    return result.returncode, result.stdout + result.stderr


def parse_latencies(output):
    results = {}
    for m in re.finditer(r"LATENCY\s+(\S+):\s+avg=([0-9.]+)", output):
        results[m.group(1)] = float(m.group(2))
    return results


def check(condition, msg):
    status = "OK  " if condition else "FAIL"
    print(f"  [{status}] {msg}")
    return condition


# ---------------------------------------------------------------------------
# main
# ---------------------------------------------------------------------------

def main():
    parser = argparse.ArgumentParser(
        description="AraXL instruction latency checker"
    )
    parser.add_argument("--gem5",     required=True,  help="path to gem5.opt")
    parser.add_argument("--script",   required=True,  help="path to riscv-rvv-se-ara.py")
    parser.add_argument("--binary",   required=True,  help="path to rvv_ara_latency_test.bin")
    parser.add_argument("--vlen",     type=int, default=1024,
                        help="VLEN in bits for standard config (default 1024)")
    parser.add_argument("--lanes",    type=int, default=4,
                        help="NrLanes for standard config (default 4)")
    parser.add_argument("--clusters", type=int, default=2,
                        help="NrClusters for standard config (default 2)")
    parser.add_argument("--tol",      type=float, default=1.5,
                        help="tolerance in cycles for each check (default 1.5)")
    parser.add_argument("--skip-throughput", action="store_true",
                        help="Skip the throughput differentiation test (faster)")
    args = parser.parse_args()

    all_ok = True

    # =======================================================================
    # Suite 1: Standard AraXL config
    # =======================================================================
    print()
    print("=" * 65)
    print("Suite 1: AraXL Standard Config")
    print(f"  VLEN={args.vlen}  NrLanes={args.lanes}  NrClusters={args.clusters}"
          f"  tolerance=±{args.tol}")
    print("=" * 65)

    rc, output = run_gem5(args.gem5, args.script, args.binary,
                          args.vlen, args.lanes, args.clusters,
                          model="araxl")
    print(output[-4000:])
    all_ok &= check(rc == 0, f"gem5 exit code 0 (got {rc})")

    measured = parse_latencies(output)
    all_ok &= check(
        len(measured) >= len(EXPECTED_STANDARD),
        f"parsed {len(measured)} LATENCY lines (expected {len(EXPECTED_STANDARD)})"
    )

    print()
    print(f"  {'Test':<18} {'Expected':>8} {'AraXL RTL':>9} {'Measured':>9}  Result")
    print(f"  {'-'*18} {'-'*8} {'-'*9} {'-'*9}  ------")

    for name, (raw_exp, rtl_val, note) in EXPECTED_STANDARD.items():
        if name not in measured:
            check(False, f"{name:<18} not found in output")
            all_ok = False
            continue
        meas = measured[name]
        ok   = abs(meas - raw_exp) <= args.tol
        result_str = "OK" if ok else f"FAIL (expected {raw_exp} ± {args.tol})"
        print(f"  {name:<18} {raw_exp:>8.1f} {rtl_val:>9}  {meas:>8.2f}  {result_str}")
        if not ok:
            print(f"    note: {note}")
        all_ok &= ok

    for name in measured:
        if name not in EXPECTED_STANDARD:
            print(f"  [WARN] unexpected LATENCY line '{name}' "
                  f"(avg={measured[name]:.2f}) — not checked")

    # =======================================================================
    # Suite 2: Throughput differentiation (NrLanes=1, VLEN=512)
    # =======================================================================
    if not args.skip_throughput:
        print()
        print("=" * 65)
        print("Suite 2: Throughput Differentiation Test  (vfdiv_ew32, NrLanes=1)")
        print("  Non-pipelined FU (count=1) serialises micro-ops → dynamicOpLatency")
        print("  ARA  (1L/1C): epc=2,  tc=8  → 8 uops × 11 cy = 88 cy total")
        print("  AraXL(1L/2C): epc=4,  tc=4  → 8 uops × 7  cy = 56 cy total")
        print("=" * 65)

        # --- ARA baseline (1 lane, 1 cluster) ---
        print("\n  [run] ARA baseline (model=ara, lanes=1, clusters=1, vlen=512)")
        rc_ara, out_ara = run_gem5(args.gem5, args.script, args.binary,
                                   512, 1, 1, model="ara")
        print(out_ara[-2000:])
        all_ok &= check(rc_ara == 0, f"ARA throughput run: gem5 exit code 0 (got {rc_ara})")
        meas_ara = parse_latencies(out_ara)

        # --- AraXL (1 lane, 2 clusters) ---
        print("\n  [run] AraXL (model=araxl, lanes=1, clusters=2, vlen=512)")
        rc_xl, out_xl = run_gem5(args.gem5, args.script, args.binary,
                                  512, 1, 2, model="araxl")
        print(out_xl[-2000:])
        all_ok &= check(rc_xl == 0, f"AraXL throughput run: gem5 exit code 0 (got {rc_xl})")
        meas_xl = parse_latencies(out_xl)

        print()
        print(f"  {'Test':<18} {'ARA exp':>8} {'ARA meas':>9} {'XL exp':>7} "
              f"{'XL meas':>8} {'ARA':>5} {'AraXL':>6}")
        print(f"  {'-'*18} {'-'*8} {'-'*9} {'-'*7} {'-'*8} {'-'*5} {'-'*6}")

        for name, (exp_ara, note_ara) in EXPECTED_THROUGHPUT_ARA.items():
            exp_xl, note_xl = EXPECTED_THROUGHPUT_ARAXL[name]
            m_ara = meas_ara.get(name, float("nan"))
            m_xl  = meas_xl.get(name, float("nan"))
            ok_ara = abs(m_ara - exp_ara) <= args.tol
            ok_xl  = abs(m_xl  - exp_xl)  <= args.tol
            print(f"  {name:<18} {exp_ara:>8.1f} {m_ara:>9.2f} "
                  f"{exp_xl:>7.1f} {m_xl:>8.2f} "
                  f"{'OK' if ok_ara else 'FAIL':>5} {'OK' if ok_xl else 'FAIL':>6}")
            if not ok_ara:
                print(f"    ARA  note: {note_ara}")
            if not ok_xl:
                print(f"    AraXL note: {note_xl}")
            all_ok &= ok_ara
            all_ok &= ok_xl

        # Also verify AraXL is faster than ARA (key correctness property)
        for name in EXPECTED_THROUGHPUT_ARA:
            m_ara = meas_ara.get(name, float("nan"))
            m_xl  = meas_xl.get(name, float("nan"))
            faster = m_xl < m_ara
            all_ok &= check(faster,
                f"{name}: AraXL ({m_xl:.2f}) faster than ARA ({m_ara:.2f}) "
                f"with 2× clusters")

    # =======================================================================
    # Summary
    # =======================================================================
    print()
    print("=" * 65)
    if all_ok:
        print("RESULT: ALL CHECKS PASSED")
    else:
        print("RESULT: ONE OR MORE CHECKS FAILED")
    print("=" * 65)

    sys.exit(0 if all_ok else 1)


if __name__ == "__main__":
    main()
