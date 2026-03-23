#!/usr/bin/env python3
import sys
import re
import math

# --- Configuration ---
DEFAULT_VLEN = 128
DEFAULT_LANES = 2

def get_expected_latency(name, sew):
    """
    Calculates expected latency using ARA hardware pipeline depth rules.
    Formula: Pipeline Depth + 2 Cycle Synchronization Overhead.
    """
    overhead = 2.0
    
    # VFU_Alu (Add, Logic, Shift, Slide, Mask)
    if name.startswith(("vadd", "vor", "vand", "vsll", "vslide", "vmseq", "vmslt", "vredsum")):
        pipe = 1
    
    # VFU_Mul (Integer Multiplier)
    elif name.startswith("vmul"):
        pipe = 0 if sew == 8 else 1
    
    # VFU_Div (Iterative Divider)
    elif name.startswith("vdiv"):
        # Scale: 4 << vsew
        vsew_val = {8:0, 16:1, 32:2, 64:3}[sew]
        pipe = 4 << vsew_val
    
    # VFU_MFpu (Floating Point)
    elif name.startswith(("vfadd", "vfmul", "vfmacc")):
        # Scale: vsew + 2
        vsew_val = {8:0, 16:1, 32:2, 64:3}[sew]
        pipe = vsew_val + 2
    
    # VFU_MFpu (FP Div/Sqrt/Cvt)
    elif name.startswith(("vfdiv", "vfsqrt")):
        pipe = 3
    elif name.startswith("vfcvt"):
        pipe = 2
    
    else:
        pipe = 1 # Default
        
    return float(pipe + overhead)

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
                    lat_results[name] = (float(cycles) / insts, sew)
                
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
        pass
    return vlen, lat_results, thru_results

def compare_results(vlen, actual_lat, actual_thru, lanes):
    print("=" * 75)
    print(f" ARA VERIFICATION: VLEN={vlen}, LANES={lanes}")
    print("-" * 75)
    print(f"{'Metric':<8} | {'Instruction':<18} | {'SEW':<4} | {'Exp':<6} | {'Act':<6} | {'Stat'}")
    print("-" * 75)
    
    passed = 0
    total = 0
    
    # 1. Check Pipeline Latencies
    for name, (act_val, sew) in actual_lat.items():
        total += 1
        exp_val = get_expected_latency(name, sew)
        delta = abs(act_val - exp_val)
        stat = "PASS" if delta < 0.15 else "FAIL"
        
        if stat == "PASS": passed += 1
        print(f"LAT      | {name:<18} | {sew:<4} | {exp_val:<6.1f} | {act_val:<6.2f} | {stat}")

    # 2. Check Throughput (Lane counts)
    for name, (act_val, sew) in actual_thru.items():
        total += 1
        exp_val = math.ceil(vlen / (lanes * sew))
        delta = abs(act_val - exp_val)
        stat = "PASS" if delta < 0.15 else "FAIL"
        
        if stat == "PASS": passed += 1
        print(f"THROUGH  | {name:<18} | {sew:<4} | {exp_val:<6.1f} | {act_val:<6.2f} | {stat}")

    print("=" * 75)
    print(f"SUMMARY: {passed}/{total} Tests Passed")
    if total > 0 and passed == total:
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
