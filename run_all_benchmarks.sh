#!/bin/bash
mkdir -p m5out

declare -A args
args["_axpy"]=""
args["_blackscholes"]=""
args["_jacobi-2d"]=""
args["_lavaMD"]=""
args["_matmul"]="apps_gem5/riscv-vectorized-benchmark-suite/_matmul/input/data_64.in"
args["_particlefilter"]="-x 128 -y 128 -z 2 -np 256"
args["_pathfinder"]=""
args["_somier"]="5 10"
args["_spmv"]=""
args["_streamcluster"]=""
args["_swaptions"]="-run"

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
    if [ -z "${args[$bench_name]}" ]; then
        docker run --rm -v $(pwd):/gem5 -w /gem5 rseac/gem5-ara:latest build/RISCV/gem5.opt run_gem5_ara.py apps_gem5/bin/$bin_name > m5out/out_${bench_name}.txt 2>&1
    else
        docker run --rm -v $(pwd):/gem5 -w /gem5 rseac/gem5-ara:latest build/RISCV/gem5.opt run_gem5_ara.py apps_gem5/bin/$bin_name "${args[$bench_name]}" > m5out/out_${bench_name}.txt 2>&1
    fi
done

echo "Parsing results:"
python3 parse_existing.py
