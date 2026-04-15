#!/bin/bash

# Simple wrapper to run gem5 with the ARA timing model.
# Usage: ./run.sh <binary_path> [kernel_name]

if [ -z "$1" ]; then
    echo "Usage: $0 <binary_path> [kernel_name]"
    exit 1
fi

BINARY=$1
KERNEL=$2

# Paths
GEM5_BIN="/gem5/build/RISCV/gem5.opt"
CONFIG_SCRIPT="./riscv-rvv-se-ara.py"

# Default ARA parameters (can be overridden by environment variables)
L1D_SIZE=${L1D_SIZE:-"32KiB"}
L2_SIZE=${L2_SIZE:-"512KiB"}
VLEN=${VLEN:-2048}
ELEN=${ELEN:-64}

echo "Running $BINARY with L1D=$L1D_SIZE, L2=$L2_SIZE, VLEN=$VLEN, ELEN=$ELEN"

# Build the command
CMD=("$GEM5_BIN" "$CONFIG_SCRIPT" \
    --cpu-type AraO3 \
    --enable-chaining \
    --simd-units 2 \
    --vector-timing-throughput 4 \
    -d "$L1D_SIZE" \
    -2 "$L2_SIZE" \
    -v "$VLEN" \
    -e "$ELEN")

# If a kernel name is provided, pass it via -p
if [ ! -z "$KERNEL" ]; then
    CMD+=("-p" "$KERNEL")
fi

# Finally add the binary
CMD+=("$BINARY")

# Execute
"${CMD[@]}"

# Extract and subtract numCycles from stats.txt if they exist
STATS_FILE="m5out/stats.txt"
if [ -f "$STATS_FILE" ]; then
    # Get the first two occurrences of numCycles
    CYCLES=($(grep "board.processor.cores.core.numCycles" "$STATS_FILE" | awk '{print $2}'))
    
    if [ ${#CYCLES[@]} -ge 2 ]; then
        ROI_CYCLES=${CYCLES[0]}
        POST_ROI=${CYCLES[1]}
        echo "--------------------------------------------------"
        echo "Gem5 Stats Analysis (m5out/stats.txt):"
        echo "  ROI Cycles (Loop):       $ROI_CYCLES"
        echo "  Post-ROI (Cleanup):      $POST_ROI"
        echo ""
        echo "  Note: ROI Cycles should match the [M5_OPS] or [RDCYCLE] "
        echo "        output printed in the terminal."
        echo "--------------------------------------------------"
    elif [ ${#CYCLES[@]} -eq 1 ]; then
        echo "--------------------------------------------------"
        echo "Gem5 Stats Analysis (m5out/stats.txt):"
        echo "  Total Cycles: ${CYCLES[0]}"
        echo "--------------------------------------------------"
    fi
fi
