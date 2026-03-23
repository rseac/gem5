#!/usr/bin/env python3
import sys
import re

# --- Gold Standard Latencies ---
# These are the expected cycles/instruction for a dependent chain.
# Calculated as: Pipeline Depth + 2 Cycle ARA Chaining Overhead.
EXPECTED_LATENCIES = {
    "vadd_e8":     3.0,  # 1 pipe + 2 overhead
    "vadd_e16":    3.0,
    "vadd_e32":    3.0,
    "vadd_e64":    3.0,
    "vor_e32":     3.0,
    "vsll_e32":    3.0,
    "vmslt_e32":   3.0,
    "vmul_e8":     2.0,  # 0 pipe + 2 overhead
    "vmul_e32":    3.0,  # 1 pipe + 2 overhead
    "vmulh_e32":   3.0,
    "vdiv_e32":    18.0, # 16 pipe (4<<2) + 2 overhead
    "vdiv_e64":    34.0, # 32 pipe (4<<3) + 2 overhead
    "vfadd_e32":   6.0,  # 4 pipe (vsew+2) + 2 overhead
    "vfadd_e64":   7.0,  # 5 pipe (vsew+2) + 2 overhead
    "vfmul_e32":   6.0,
    "vfmacc_e32":  6.0,
    "vfdiv_e32":   5.0,  # 3 pipe + 2 overhead
    "vfsqrt_e32":  5.0,
    "vfcvt_e32":   4.0,  # 2 pipe + 2 overhead
    "vslide_e32":  3.0,  # 1 pipe + 2 overhead
    "vredsum_e32": 3.0,
}

import math

# --- Configuration ---
# You can override these via command line if your hardware differs
DEFAULT_VLEN = 128
DEFAULT_LANES = 2

# --- Gold Standard Latencies ---
# Expected Pipeline Depth + 2 Cycle Overhead
LATENCY_EXPECTS = {
    "vadd_e8":     3.0, "vadd_e16":    3.0, "vadd_e32":    3.0, "vadd_e64":    3.0,
    "vmul_e8":     2.0, "vmul_e32":    3.0, "vmul_e64":    3.0,
    "vdiv_e32":    18.0, "vdiv_e64":   34.0,
    "vfadd_e32":   6.0, "vfadd_e64":   7.0,
    "vfmul_e32":   6.0, "vfdiv_e32":   5.0, "vfsqrt_e32":  5.0,
    "vslide_e32":  3.0,
}

def parse_rtl_output(file_path):
    lat_results = {}
    thru_results = {}
    vlen = DEFAULT_VLEN
    
    try:
        with open(file_path, 'r') as f:
            for line in f:
                parts = line.split()
                if not parts: continue
                
                # Format: VLEN_CHECK: 4096
                if parts[0] == "VLEN_CHECK:":
                    vlen = int(parts[1])
                
                # Format: DATA_POINT vadd_e32 32 3000 1000
                if parts[0] == "DATA_POINT":
                    name = parts[1]
                    sew = int(parts[2])
                    cycles = int(parts[3])
                    insts = int(parts[4])
                    lat_results[name] = float(cycles) / insts
                
                # Format: DATA_THROUGH vadd_thru_e32 32 1000 1000
                if parts[0] == "DATA_THROUGH":
                    name = parts[1]
                    sew = int(parts[2])
                    cycles = int(parts[3])
                    insts = int(parts[4])
                    thru_results[name] = (float(cycles) / insts, sew)
                    
    except FileNotFoundError:
        print(f"Error: Could not find result file '{file_path}'")
        sys.exit(1)
    except (ValueError, IndexError):
        # Skip malformed lines
        pass
    return vlen, lat_results, thru_results

def compare_results(vlen, actual_lat, actual_thru, lanes):
    print("=" * 70)
    print(f" ARA VERIFICATION: VLEN={vlen}, LANES={lanes}")
    print("-" * 70)
    print(f"{'Metric':<8} | {'Instruction':<18} | {'Exp':<8} | {'Act':<8} | {'Delta':<6} | {'Stat'}")
    print("-" * 70)
    
    passed = 0
    total = 0
    
    # 1. Check Pipeline Latencies
    for name, exp_val in LATENCY_EXPECTS.items():
        total += 1
        if name in actual_lat:
            act_val = actual_lat[name]
            delta = abs(act_val - exp_val)
            stat = "PASS" if delta < 0.15 else "FAIL"
            if stat == "PASS": passed += 1
            print(f"LAT      | {name:<18} | {exp_val:<8.2f} | {act_val:<8.2f} | {act_val-exp_val:<+6.2f} | {stat}")
        else:
            print(f"LAT      | {name:<18} | {exp_val:<8.2f} | {'MISS':<8} | {'-':<6} | SKIP")

    # 2. Check Throughput (Lane counts)
    for name, (act_val, sew) in actual_thru.items():
        total += 1
        # Exp Throughput = ceil(VLEN / (Lanes * SEW))
        exp_val = math.ceil(vlen / (lanes * sew))
        delta = abs(act_val - exp_val)
        stat = "PASS" if delta < 0.15 else "FAIL"
        if stat == "PASS": passed += 1
        print(f"THROUGH  | {name:<18} | {exp_val:<8.2f} | {act_val:<8.2f} | {act_val-exp_val:<+6.2f} | {stat}")

    print("=" * 70)
    print(f"SUMMARY: {passed}/{total} Tests Passed")
    if passed == total:
        print("SUCCESS: ARA Hardware matches gem5 configuration.")
    else:
        print("FAILURE: Discrepancies detected.")

if __name__ == "__main__":
    if len(sys.argv) < 2:
        print("Usage: ./check_rtl_results.py <log> [lanes_override]")
        sys.exit(1)
    
    lanes = int(sys.argv[2]) if len(sys.argv) > 2 else DEFAULT_LANES
    vlen, a_lat, a_thru = parse_rtl_output(sys.argv[1])
    compare_results(vlen, a_lat, a_thru, lanes)
