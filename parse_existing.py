import os, sys, csv
import numpy as np
import re

BASELINE_CSV = "/home/twiga/code/github/ai/my-timing-project/benchmark_suite/ara/results_4L_4096V/status.csv"
BENCHMARK_MAP = {
    "_axpy": "axpy_se.exe",
    "_blackscholes": "blackscholes_se.exe",
    "_pathfinder": "pathfinder_se.exe",
    "_streamcluster": "streamcluster_se.exe",
    "_spmv": "spmv_se.exe",
    "_swaptions": "swaptions_se.exe",
    "_lavaMD": "lavaMD_se.exe",
    "_matmul": "matmul_se.exe",
    "_jacobi-2d": "jacobi2d_se.exe",
    "_particlefilter": "particlefilter_se.exe",
    "_somier": "somier_se.exe"
}

baseline_cycles = {}
with open(BASELINE_CSV, 'r') as f:
    reader = csv.DictReader(f)
    for row in reader:
        b = row['Benchmark']
        if b in BENCHMARK_MAP:
            baseline_cycles[b] = int(row['ROI_Cycles'])

errors = []
print(f"{'Benchmark':<20} | {'Ara RTL (ROI)':<15} | {'gem5':<10} | {'Error %':<10}")
print("-" * 65)

for b in BENCHMARK_MAP:
    out_file = f"m5out/out_{b}.txt"
    cycles = 0
    if os.path.exists(out_file):
        with open(out_file, "r") as f:
            for line in f:
                if "[ROI-LATENCY]:" in line or "Time:" in line:
                    m = re.search(r'(\d+)\s+cycles', line)
                    if m:
                        cycles = int(m.group(1))
                    break
    
    rtl_cyc = baseline_cycles.get(b, 0)
    if rtl_cyc > 0 and cycles > 0:
        err = abs(cycles - rtl_cyc) / rtl_cyc * 100.0
        errors.append(err)
        print(f"{b:<20} | {rtl_cyc:<15} | {cycles:<10} | {err:.2f}%")
    else:
        print(f"{b:<20} | {rtl_cyc:<15} | {cycles:<10} | ERR")

if errors:
    print("-" * 65)
    print(f"MAPE: {np.mean(errors):.2f}%")
