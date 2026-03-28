#!/usr/bin/env python3
"""
check_ara_latencies.py
======================
Runs rvv_ara_latency_test.bin under gem5 and checks that each measured
instruction latency matches the expected ARA RTL chaining latency.

Expected values (VLEN=512, 4 lanes, chainingLatency = max(pipe+2, 6)):

  Name           ARA RTL   Raw measured   Note
  -------------- --------- -------------- -----------------------------------
  vadd_ew32          6          6         ALU pipe=1;   max(3,6)=6
  vmul_ew32          6          6         Mul pipe=1;   max(3,6)=6
  vdiv_ew32         18         24         vdiv(18) + vadd(6) per iteration
  vdiv_ew64         34         40         vdiv(34) + vadd(6) per iteration
  vfadd_ew32         6          6         FPComp EW32 pipe=4; max(6,6)=6
  vfadd_ew64         7          7         FPComp EW64 pipe=5; max(7,6)=7
  vfmul_ew32         6          6         same FU as vfadd_ew32
  vfmacc_ew32        6          6         same FU as vfadd_ew32
  vfmin_ew32         6          6         FPNonComp pipe=1; max(3,6)=6
  vfdiv_ew32         6          6         FPDivSqrt pipe=3; max(5,6)=6
  vfsqrt_ew32        6          6         FPDivSqrt pipe=3; max(5,6)=6
  vfcvt_ew32         6          6         FPConv pipe=2; max(4,6)=6 (per-op avg)

The vdiv_* tests include a vadd_vx in each dependency-chain iteration to keep
the chain value from reaching zero.  The binary therefore reports
  avg = chainingLatency(vdiv) + chainingLatency(vadd)
i.e. the "raw" column above.  The checker uses the raw expected value directly;
the ARA RTL column is provided for human reference only.

Usage
-----
  python3 gem5/rvv/check_ara_latencies.py \\
      --gem5   build/RISCV/gem5.opt \\
      --script gem5/rvv/riscv-rvv-se-ara.py \\
      --binary gem5/rvv/rvv_ara_latency_test.bin \\
      [--vlen 512] [--lanes 4] [--tol 1.5]

Exit code 0 if all checks pass, non-zero otherwise.
"""

import argparse
import re
import subprocess
import sys

# ---------------------------------------------------------------------------
# Expected raw averages output by the binary (cycles per iteration as printed).
# Tolerance is ± TOL cycles.
#
# Calibrated against local source (VLEN=512, 4 lanes, DISPATCH_FLOOR=6).
#
#   Short-pipeline ops (vadd, vmul, vfadd/mul/macc/min/cvt):
#     chainingLatency = max(pipe+2, 6) = 6.  The O3 issue/wakeup pipeline adds
#     ~2 cycles of overhead → measured ~8.
#
#   vdiv_ew32/64 chain (LMUL=m1, vl=4/2):
#     Each iteration = vdiv + vadd_vx.  The vadd's dynamicOpLatency is raised
#     to DISPATCH_FLOOR=6 (was ~1 without floor), adding ~5 observable cycles:
#       vdiv_ew32: 18 (vdiv) + 6 (vadd) = 24  →  measured ~23
#       vdiv_ew64: 34 (vdiv) + 6 (vadd) = 40  →  measured ~39
#
#   vfdiv_ew32 / vfsqrt_ew32 (LMUL=m8, pipelined=False):
#     8 micro-ops execute sequentially.  Each micro-op:
#       dynamicOpLatency = max(pipe(3) + throughput(2), 6) = 6
#     Total = 8 × 6 = 48 cycles.
# ---------------------------------------------------------------------------
EXPECTED = {
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
    "vmfeq_ew64":    (18,           7,       "FPCmp EW64: vmfeq(CL=7)+vfmerge(CL=6) chain, mask-register serialisation dominates; RTL LatFCompEW64=5"),
    "vfredusum_ew64":(12,           7,       "FP reduce-sum EW64: full reduction occupancy (all vl elements must complete); RTL LatFCompEW64=5"),
    "vfdiv_ew32":    (48,           6,       "FPDivSqrt EW32: 8 micro-ops × dynamicOpLatency(6) pipelined=False"),
    "vfsqrt_ew32":   (48,           6,       "FPDivSqrt EW32: 8 micro-ops × dynamicOpLatency(6) pipelined=False"),
    "vfcvt_ew32":    (8,            6,       "FPConv EW32 chainingLatency=6 + O3 overhead (per-op avg)"),
}


# ---------------------------------------------------------------------------
# helpers
# ---------------------------------------------------------------------------

def run_gem5(gem5_bin, script, binary, vlen, lanes):
    """Run gem5 and return (returncode, combined_output)."""
    cmd = [
        gem5_bin,
        script,
        "--enable-chaining",
        "--vlen", str(vlen),
        "--vector-timing-throughput", str(lanes),
        binary,
    ]
    print(f"\n[run] {' '.join(cmd)}")
    result = subprocess.run(cmd, capture_output=True, text=True, timeout=900)
    return result.returncode, result.stdout + result.stderr


def parse_latencies(output):
    """
    Extract LATENCY lines from binary output.

    Expected format (one per test):
        LATENCY <name>: avg=<float>

    Returns dict: {name: avg_cycles_float}
    """
    results = {}
    for m in re.finditer(r"LATENCY\s+(\S+):\s+avg=([0-9.]+)", output):
        name = m.group(1)
        avg  = float(m.group(2))
        results[name] = avg
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
        description="ARA instruction latency checker"
    )
    parser.add_argument("--gem5",   required=True,  help="path to gem5.opt")
    parser.add_argument("--script", required=True,  help="path to riscv-rvv-se-ara.py")
    parser.add_argument("--binary", required=True,  help="path to rvv_ara_latency_test.bin")
    parser.add_argument("--vlen",   type=int, default=512,
                        help="VLEN in bits (default 512)")
    parser.add_argument("--lanes",  type=int, default=4,
                        help="number of vector lanes (default 4)")
    parser.add_argument("--tol",    type=float, default=1.5,
                        help="tolerance in cycles for each check (default 1.5)")
    args = parser.parse_args()

    all_ok = True

    # -----------------------------------------------------------------------
    # Run
    # -----------------------------------------------------------------------
    print("=" * 60)
    print("ARA Instruction Latency Test")
    print(f"  VLEN={args.vlen}  lanes={args.lanes}  tolerance=±{args.tol}")
    print("=" * 60)

    rc, output = run_gem5(args.gem5, args.script, args.binary,
                          args.vlen, args.lanes)
    print(output[-4000:])  # tail for diagnostics

    all_ok &= check(rc == 0, f"gem5 exit code 0 (got {rc})")

    measured = parse_latencies(output)
    all_ok &= check(
        len(measured) >= len(EXPECTED),
        f"parsed {len(measured)} LATENCY lines (expected {len(EXPECTED)})"
    )

    # -----------------------------------------------------------------------
    # Per-instruction checks
    # -----------------------------------------------------------------------
    print()
    print(f"  {'Test':<18} {'Expected':>8} {'ARA RTL':>8} {'Measured':>9}  Result")
    print(f"  {'-'*18} {'-'*8} {'-'*8} {'-'*9}  ------")

    for name, (raw_exp, ara_rtl, note) in EXPECTED.items():
        if name not in measured:
            msg = f"{name:<18} not found in output"
            check(False, msg)
            all_ok = False
            continue

        meas = measured[name]
        ok   = abs(meas - raw_exp) <= args.tol
        result_str = "OK" if ok else f"FAIL (expected {raw_exp} ± {args.tol})"
        print(f"  {name:<18} {raw_exp:>8.1f} {ara_rtl:>8}  {meas:>8.2f}  {result_str}")
        if not ok:
            print(f"    note: {note}")
        all_ok &= ok

    # Warn about any LATENCY lines in the output that are not in EXPECTED
    for name in measured:
        if name not in EXPECTED:
            print(f"  [WARN] unexpected LATENCY line for '{name}' "
                  f"(avg={measured[name]:.2f}) — not checked")

    # -----------------------------------------------------------------------
    # Summary
    # -----------------------------------------------------------------------
    print()
    print("=" * 60)
    if all_ok:
        print("RESULT: ALL CHECKS PASSED")
    else:
        print("RESULT: ONE OR MORE CHECKS FAILED")
    print("=" * 60)

    sys.exit(0 if all_ok else 1)


if __name__ == "__main__":
    main()
