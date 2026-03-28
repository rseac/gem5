#!/usr/bin/env python3
"""
run_load_chain_test.py
======================
Runs rvv_load_chain_test.bin under gem5 with load chaining enabled and
disabled, then checks:

  1. Correctness  – every test case in both runs reports "PASS".
  2. Timing       – chained vle32->vfmul and vle32->vfmacc must complete in
                    ≤ (no-chain cycles + tolerance) when chaining is on.
                    Concretely: with chaining the avg-cycles values reported
                    by the binary for Tests 1 & 2 must be ≤ those of the
                    no-chain run (or within TIMING_SLACK cycles of them in
                    case the working-set fits in L1 and the difference is
                    small).

Usage
-----
  # from the repo root (adjust paths as needed):
  python3 gem5/rvv/run_load_chain_test.py \
      --gem5  build/RISCV/gem5.opt \
      --script gem5/rvv/riscv-rvv-se-ara.py \
      --binary gem5/rvv/rvv_load_chain_test.bin \
      [--vlen 512] [--lanes 4]

The script exits with code 0 if all checks pass, non-zero otherwise.
"""

import argparse
import re
import subprocess
import sys

# How many extra cycles we tolerate for the chained case vs the unchained
# case before declaring a regression (set generously to handle LMUL=m8
# with small working sets where the overhead difference can be small).
TIMING_SLACK = 10


# ---------------------------------------------------------------------------
# helpers
# ---------------------------------------------------------------------------

def run_gem5(gem5_bin, script, binary, vlen, lanes, chaining):
    """Run gem5 and return (returncode, stdout_text)."""
    chain_flag = "--enable-chaining" if chaining else "--disable-chaining"
    cmd = [
        gem5_bin,
        script,
        chain_flag,
        "--vlen", str(vlen),
        "--vector-timing-throughput", str(lanes),
        binary,
    ]
    print(f"\n[run] {' '.join(cmd)}")
    result = subprocess.run(cmd, capture_output=True, text=True, timeout=600)
    return result.returncode, result.stdout + result.stderr


def parse_results(output):
    """
    Extract per-test avg cycles and pass/fail from the binary output.

    Expected lines (produced by rvv_load_chain_test.c):
      avg cycles/iter = 42.0
      correctness: PASS
    Returns list of dicts: [{'avg': float, 'pass': bool}, ...]  (3 tests)
    """
    tests = []
    avgs  = re.findall(r"avg cycles/iter\s*=\s*([\d.]+)", output)
    oks   = re.findall(r"correctness:\s*(PASS|FAIL)", output)

    for i, avg_str in enumerate(avgs):
        result = {
            "avg":  float(avg_str),
            "pass": True,          # baseline test has no correctness line
        }
        if i < len(oks):
            result["pass"] = (oks[i] == "PASS")
        tests.append(result)

    return tests


def check(condition, msg):
    if not condition:
        print(f"  FAIL: {msg}")
        return False
    print(f"  OK:   {msg}")
    return True


# ---------------------------------------------------------------------------
# main
# ---------------------------------------------------------------------------

def main():
    parser = argparse.ArgumentParser(description="Load-chain test runner")
    parser.add_argument("--gem5",   required=True,  help="path to gem5.opt")
    parser.add_argument("--script", required=True,  help="path to riscv-rvv-se-ara.py")
    parser.add_argument("--binary", required=True,  help="path to rvv_load_chain_test.bin")
    parser.add_argument("--vlen",   type=int, default=512, help="VLEN in bits (default 512)")
    parser.add_argument("--lanes",  type=int, default=4,   help="vector lanes / throughput (default 4)")
    args = parser.parse_args()

    all_ok = True

    # -----------------------------------------------------------------------
    # Run with chaining ENABLED
    # -----------------------------------------------------------------------
    print("\n" + "=" * 60)
    print("Run 1: load chaining ENABLED")
    print("=" * 60)
    rc_on, out_on = run_gem5(args.gem5, args.script, args.binary,
                              args.vlen, args.lanes, chaining=True)
    print(out_on[-3000:])   # tail of output for diagnostics

    if not check(rc_on == 0, f"gem5 exit code 0 (got {rc_on})"):
        all_ok = False

    tests_on = parse_results(out_on)
    if not check(len(tests_on) >= 2,
                 f"parsed ≥2 test results (got {len(tests_on)})"):
        all_ok = False
        tests_on = [{"avg": float("inf"), "pass": False}] * 3

    for i, t in enumerate(tests_on):
        if "pass" in t and i < 2:          # Test 3 has no correctness check
            all_ok &= check(t["pass"],
                            f"Test {i+1} correctness (chaining=on)")

    # -----------------------------------------------------------------------
    # Run with chaining DISABLED
    # -----------------------------------------------------------------------
    print("\n" + "=" * 60)
    print("Run 2: load chaining DISABLED")
    print("=" * 60)
    rc_off, out_off = run_gem5(args.gem5, args.script, args.binary,
                                args.vlen, args.lanes, chaining=False)
    print(out_off[-3000:])

    if not check(rc_off == 0, f"gem5 exit code 0 (got {rc_off})"):
        all_ok = False

    tests_off = parse_results(out_off)
    if not check(len(tests_off) >= 2,
                 f"parsed ≥2 test results (got {len(tests_off)})"):
        all_ok = False
        tests_off = [{"avg": float("inf"), "pass": False}] * 3

    for i, t in enumerate(tests_off):
        if "pass" in t and i < 2:
            all_ok &= check(t["pass"],
                            f"Test {i+1} correctness (chaining=off)")

    # -----------------------------------------------------------------------
    # Timing comparison: chaining should not make things slower
    # -----------------------------------------------------------------------
    print("\n" + "=" * 60)
    print("Timing comparison (chaining on vs off)")
    print("=" * 60)

    test_names = [
        "vle32->vfmul   (Test 1)",
        "vle32->vfmacc  (Test 2)",
        "vfmul baseline (Test 3)",
    ]

    for i, name in enumerate(test_names):
        if i >= len(tests_on) or i >= len(tests_off):
            break
        cyc_on  = tests_on[i]["avg"]
        cyc_off = tests_off[i]["avg"]
        print(f"  {name}: chain={cyc_on:.1f}  no-chain={cyc_off:.1f}")

        if i < 2:
            # For load-chained tests: chaining should be ≤ no-chain + slack
            ok = cyc_on <= cyc_off + TIMING_SLACK
            all_ok &= check(ok,
                f"{name}: chaining ({cyc_on:.1f}) ≤ no-chain ({cyc_off:.1f}) + {TIMING_SLACK}")
        else:
            # Baseline test should be identical (no load in chain)
            ok = abs(cyc_on - cyc_off) <= TIMING_SLACK
            all_ok &= check(ok,
                f"{name}: baseline unchanged within {TIMING_SLACK} cycles")

    # -----------------------------------------------------------------------
    # Summary
    # -----------------------------------------------------------------------
    print("\n" + "=" * 60)
    if all_ok:
        print("RESULT: ALL CHECKS PASSED")
    else:
        print("RESULT: ONE OR MORE CHECKS FAILED")
    print("=" * 60 + "\n")

    sys.exit(0 if all_ok else 1)


if __name__ == "__main__":
    main()
