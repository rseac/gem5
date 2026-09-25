import os
import sys
import csv
import subprocess
import re
import numpy as np

# RIVEC Benchmarks to test
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

BENCHMARK_ARGS = {
    "_blackscholes": "input/in_16K.input",
    "_pathfinder": "input/data_medium.in",
    "_streamcluster": "3 3 128 8 8 10",
    "_spmv": "input/lhr07.mtx",
    "_lavaMD": "1 1 32",
    "_matmul": "128 128 128",
    "_particlefilter": "128 128 128",
    "_somier": "100 100",
    "_swaptions": "128 1"
}

BINS_DIR = "/home/twiga/code/github/ai/tunable-gem5/gem5/apps_gem5/bin"
BASELINE_CSV = "/home/twiga/code/github/ai/my-timing-project/benchmark_suite/ara/results_4L_4096V/status.csv"

# 1. Read baseline RTL cycles
baseline_cycles = {}
with open(BASELINE_CSV, 'r') as f:
    reader = csv.DictReader(f)
    for row in reader:
        b = row['Benchmark']
        if b in BENCHMARK_MAP:
            baseline_cycles[b] = int(row['ROI_Cycles']) # Use ROI_Cycles since Ara gem5 doesn't count full baremetal boot

gem5_cycles = {}

# 2. Run gem5 and extract cycles
print(f"{'Benchmark':<20} | {'Ara RTL (ROI)':<15} | {'gem5':<10} | {'Error %':<10}")
print("-" * 65)

errors = []

for b, elf_name in BENCHMARK_MAP.items():
    elf_path = os.path.join(BINS_DIR, elf_name)
    if not os.path.exists(elf_path):
        print(f"Skipping {b}, no elf found.")
        continue
    # Path as seen inside the container, where $(pwd) is mounted at /gem5 -
    # the host-absolute elf_path above doesn't exist in there.
    container_elf_path = os.path.relpath(elf_path, os.getcwd())
    args = BENCHMARK_ARGS.get(b, "")

    # Run gem5 via Docker
    cmd = f"docker run --rm -u $(id -u):$(id -g) -v $(pwd):/gem5 -v /home/twiga/code/github/ai/my-timing-project:/home/twiga/code/github/ai/my-timing-project -w /gem5 rseac/gem5-ara:latest build/RISCV/gem5.opt run_gem5_ara.py {container_elf_path} {args} > m5out/out_{b}.txt 2>&1"

    subprocess.run(cmd, shell=True)
    
    # Parse m5out/out_{b}.txt for [ROI-LATENCY]
    cycles = 0
    out_file = f"m5out/out_{b}.txt"
    if os.path.exists(out_file):
        with open(out_file, "r") as f:
            for line in f:
                if "[ROI-LATENCY]:" in line or "Time:" in line:
                    # Expected format: [ROI-LATENCY]: 41725 cycles
                    parts = line.split()
                    import re
                    m = re.search(r'(\d+)\s+cycles', line)
                    if m:
                        cycles = int(m.group(1))
                    break
                    
    gem5_cycles[b] = cycles
    
    rtl_cyc = baseline_cycles.get(b, 0)
    
    if rtl_cyc > 0 and cycles > 0:
        err = abs(cycles - rtl_cyc) / rtl_cyc * 100.0
        errors.append(err)
        print(f"{b:<20} | {rtl_cyc:<10} | {cycles:<10} | {err:.2f}%")
    else:
        print(f"{b:<20} | {rtl_cyc:<10} | {cycles:<10} | ERR")

if errors:
    mape = np.mean(errors)
    print("-" * 60)
    print(f"MAPE: {mape:.2f}%")
else:
    print("No valid data collected.")
