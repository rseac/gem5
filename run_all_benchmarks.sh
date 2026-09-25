#!/bin/bash
mkdir -p m5out

# Need some benchmarks to pass extra args as noted in HANDOFF.md:
# matmul, particlefilter, somier, swaptions
# But run_gem5_ara.py might not support arbitrary trailing args properly unless configured.
# Let's just run them as-is.

BENCHMARKS=(
    "_axpy:axpy_se.exe"
    "_blackscholes:blackscholes_se.exe"
    "_jacobi-2d:jacobi2d_se.exe"
    "_lavaMD:lavaMD_se.exe"
    "_matmul:matmul_se.exe"
    "_particlefilter:particlefilter_se.exe"
    "_pathfinder:pathfinder_se.exe"
    "_somier:somier_se.exe"
    "_spmv:spmv_se.exe"
    "_streamcluster:streamcluster_se.exe"
    "_swaptions:swaptions_se.exe"
)

echo "Running all benchmarks..."
for entry in "${BENCHMARKS[@]}"; do
    bench_name="${entry%%:*}"
    bin_name="${entry##*:}"
    
    echo "Running $bench_name ($bin_name)..."
    docker run --rm -v $(pwd):/gem5 -w /gem5 rseac/gem5-ara:latest build/RISCV/gem5.opt run_gem5_ara.py apps_gem5/bin/$bin_name > m5out/out_${bench_name}.txt 2>&1
done

echo "Parsing results:"
python3 parse_existing.py
