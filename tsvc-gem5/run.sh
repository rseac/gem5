#!/bin/bash

# Simple wrapper to run gem5 with the ARA timing model.
# Usage: ./run.sh <binary_path> [kernel_name]

if [ -z "$1" ]; then
    echo "Usage: $0 <binary_path> [kernel_name]"
    exit 1
fi

BINARY=$1
shift
KERNELS=$@

# Paths
GEM5_BIN="../build/RISCV/gem5.opt"
CONFIG_SCRIPT="./riscv-rvv-se-ara.py"

# Default ARA parameters (can be overridden by environment variables)
L1D_SIZE=${L1D_SIZE:-"32KiB"}
L2_SIZE=${L2_SIZE:-"512KiB"}
VLEN=${VLEN:-2048}
ELEN=${ELEN:-64}
CPU_TYPE=${CPU_TYPE:-"AraO3"}
LANES=${LANES:-2}
ITERATIONS=${ITERATIONS:-1}
TRACE_VEC=${TRACE_VEC:-0}

echo "Running $BINARY with L1D=$L1D_SIZE, L2=$L2_SIZE, VLEN=$VLEN, ELEN=$ELEN, CPU=$CPU_TYPE, LANES=$LANES, ITERATIONS=$ITERATIONS"
if [ "$TRACE_VEC" -eq 1 ]; then
    echo "Vector execution tracing ENABLED"
fi
echo "Kernels: ${KERNELS:-all}"

# Build the command
CMD=("$GEM5_BIN" "--quiet")

if [ "$TRACE_VEC" -eq 1 ]; then
    CMD+=("--debug-flags=ExecVector")
fi

CMD+=("$CONFIG_SCRIPT" \
    --cpu-type "$CPU_TYPE" \
    --enable-chaining \
    --simd-units 2 \
    --vector-timing-throughput "$LANES" \
    -d "$L1D_SIZE" \
    -2 "$L2_SIZE" \
    -v "$VLEN" \
    -e "$ELEN")

# Program parameters (kernel names and iterations)
PROG_ARGS=""
if [ ! -z "$KERNELS" ]; then
    PROG_ARGS="$KERNELS"
fi
PROG_ARGS="$PROG_ARGS -i $ITERATIONS"

# Pass program args via -p
CMD+=("-p" "$PROG_ARGS")

# Finally add the binary
CMD+=("$BINARY")

# Execute, filtering out the noisy rounding warnings
"${CMD[@]}" 2> >(grep -v -E "rounding error > tolerance|[0-9]+\.[0-9]+ rounded to [0-9]+" >&2)

# Extract ROI stats from stats.txt if they exist
# In this setup, stats are dumped twice: 
# 1st dump: ROI_BEGIN -> ROI_END (Kernel/Loop execution)
STATS_FILE="m5out/stats.txt"
if [ -f "$STATS_FILE" ]; then
    # Get all occurrences of the stats
    ALL_CYCLES=($(grep "board.processor.cores.core.numCycles" "$STATS_FILE" | awk '{print $2}'))
    ALL_VEC_INSTS=($(grep "board.processor.cores.core.commitStats0.numVecInsts" "$STATS_FILE" | awk '{print $2}'))
    
    # We want the first occurrence (the ROI block)
    if [ ${#ALL_CYCLES[@]} -ge 2 ]; then
        ROI_CYCLES=${ALL_CYCLES[0]}
        ROI_VEC_INSTS=${ALL_VEC_INSTS[0]}
        echo "--------------------------------------------------"
        echo "Gem5 Stats Analysis (m5out/stats.txt):"
        echo "  ROI Cycles (Loop):       $ROI_CYCLES"
        echo "  ROI Vector Insts:        $ROI_VEC_INSTS"
        echo "--------------------------------------------------"
    elif [ ${#ALL_CYCLES[@]} -eq 1 ]; then
        echo "--------------------------------------------------"
        echo "Gem5 Stats Analysis (m5out/stats.txt):"
        echo "  Total Cycles: ${ALL_CYCLES[0]}"
        echo "  Total Vector Insts: ${ALL_VEC_INSTS[0]}"
        echo "--------------------------------------------------"
    fi
fi
