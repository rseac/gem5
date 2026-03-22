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

def parse_rtl_output(file_path):
    results = {}
    # Pattern: LAT [vadd_e8       ] SEW=8 :   3.00
    pattern = re.compile(r"LAT\s+\[([\w_]+)\s+\]\s+SEW=\d+\s*:\s+([\d\.]+)")
    
    try:
        with open(file_path, 'r') as f:
            for line in f:
                match = pattern.search(line)
                if match:
                    name = match.group(1).strip()
                    value = float(match.group(2))
                    results[name] = value
    except FileNotFoundError:
        print(f"Error: Could not find result file '{file_path}'")
        sys.exit(1)
    return results

def compare_results(actual):
    print("=" * 65)
    print(f"{'Instruction':<20} | {'Expected':<10} | {'Actual':<10} | {'Delta':<8} | {'Status'}")
    print("-" * 65)
    
    passed = 0
    total = 0
    
    for name, exp_val in EXPECTED_LATENCIES.items():
        if name in actual:
            total += 1
            act_val = actual[name]
            delta = abs(act_val - exp_val)
            status = "PASS" if delta < 0.15 else "FAIL" # Allowing minor jitter
            
            if status == "PASS": passed += 1
            
            print(f"{name:<20} | {exp_val:<10.2f} | {act_val:<10.2f} | {act_val-exp_val:<+8.2f} | {status}")
        else:
            print(f"{name:<20} | {exp_val:<10.2f} | {'MISSING':<10} | {'-':<8} | SKIP")

    print("=" * 65)
    print(f"VERIFICATION SUMMARY: {passed}/{total} Passed")
    if passed == total:
        print("RESULT: ARA Hardware Latencies match gem5 AraO3 configuration.")
    else:
        print("RESULT: Discrepancies detected. gem5 model calibration required.")

if __name__ == "__main__":
    if len(sys.argv) < 2:
        print("Usage: ./check_rtl_results.py <rtl_sim_output.log>")
        sys.exit(1)
    
    actual_results = parse_rtl_output(sys.argv[1])
    compare_results(actual_results)
