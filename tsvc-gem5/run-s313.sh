#!/bin/bash

# ==============================================================================
# run-s313.sh
# Runs the TSVC benchmark for kernel function 's313' sweeping across:
#   - Configs: TINY, SMALL
#   - Lanes: 2, 4, 8
#   - VLEN: 1024, 2048, 4096
# under gem5 with the ARA timing model inside the gem5-ara:latest Docker image.
# ==============================================================================

# Script directory
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
GEM5_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"

# Target test kernel
TESTS=(
    "s313"
)

# Configuration array: TINY and SMALL (can be overridden by argument or CONFIGS env)
if [ ! -z "$1" ]; then
    CONFIGS=("$1")
elif [ -z "$CONFIGS" ]; then
    CONFIGS=("TINY" "SMALL")
else
    read -r -a CONFIGS <<< "$CONFIGS"
fi

# Lane counts (vector-timing-throughput) to sweep
if [ -z "$LANE_COUNTS" ]; then
    LANE_COUNTS=(2 4 8)
else
    read -r -a LANE_COUNTS <<< "$LANE_COUNTS"
fi

# VLEN options to sweep
if [ -z "$VLEN_OPTIONS" ]; then
    VLEN_OPTIONS=(1024 2048 4096)
else
    read -r -a VLEN_OPTIONS <<< "$VLEN_OPTIONS"
fi

# Default iteration count(s)
if [ -z "$ITER_COUNTS" ]; then
    ITER_COUNTS=(1)
else
    read -r -a ITER_COUNTS <<< "$ITER_COUNTS"
fi

# ARA Core & Memory Parameters
export L1D_SIZE=${L1D_SIZE:-"128KiB"}
export L2_SIZE=${L2_SIZE:-"256KiB"}
export ELEN=${ELEN:-64}
export CPU_TYPE=${CPU_TYPE:-"AraO3"}
export TRACE_VEC=${TRACE_VEC:-0}

# Docker image configuration
DOCKER_IMAGE=${DOCKER_IMAGE:-"gem5-ara:latest"}
# Fallback to rseac/gem5-ara:latest if gem5-ara:latest is not present locally
if ! docker image inspect "$DOCKER_IMAGE" >/dev/null 2>&1 && docker image inspect "rseac/gem5-ara:latest" >/dev/null 2>&1; then
    DOCKER_IMAGE="rseac/gem5-ara:latest"
fi

# ------------------------------------------------------------------------------
# Check if running inside container or on host
# ------------------------------------------------------------------------------
if [ ! -f "/.dockerenv" ] && [ "$RUN_INSIDE_CONTAINER" != "1" ]; then
    echo "=================================================="
    echo " Launching s313 TSVC sweep inside Docker: $DOCKER_IMAGE"
    echo "=================================================="
    echo "Configs:  ${CONFIGS[*]}"
    echo "Lanes:    ${LANE_COUNTS[*]}"
    echo "VLENs:    ${VLEN_OPTIONS[*]}"
    echo "gem5 root: $GEM5_ROOT"
    
    # Run container with repository mounted at /gem5
    docker run --rm -i \
        -v "${GEM5_ROOT}:/gem5" \
        -w /gem5/tsvc-gem5 \
        -e RUN_INSIDE_CONTAINER=1 \
        -e CONFIGS="${CONFIGS[*]}" \
        -e LANE_COUNTS="${LANE_COUNTS[*]}" \
        -e VLEN_OPTIONS="${VLEN_OPTIONS[*]}" \
        -e ITER_COUNTS="${ITER_COUNTS[*]}" \
        -e L1D_SIZE="$L1D_SIZE" \
        -e L2_SIZE="$L2_SIZE" \
        -e ELEN="$ELEN" \
        -e CPU_TYPE="$CPU_TYPE" \
        -e TRACE_VEC="$TRACE_VEC" \
        -e RESULTS_DIR="$RESULTS_DIR" \
        "$DOCKER_IMAGE" \
        bash -c "./run-s313.sh"
    
    EXIT_CODE=$?
    exit $EXIT_CODE
fi

# ------------------------------------------------------------------------------
# In-Container Execution Flow
# ------------------------------------------------------------------------------
cd "$SCRIPT_DIR"

# Results directory: Use env var if provided, otherwise timestamped default
if [ -z "$RESULTS_DIR" ]; then
    TIMESTAMP=$(date +%Y%m%d_%H%M%S)
    RESULTS_DIR="subset_results_sweep_s313_$TIMESTAMP"
fi
mkdir -p "$RESULTS_DIR"

echo "=================================================="
echo "Starting TSVC s313 parameter sweep"
echo "Configs:     ${CONFIGS[*]}"
echo "Lanes:       ${LANE_COUNTS[*]}"
echo "VLENs:       ${VLEN_OPTIONS[*]}"
echo "Iterations:  ${ITER_COUNTS[*]}"
echo "Tests:       ${TESTS[*]}"
echo "Results dir: $RESULTS_DIR"
echo "=================================================="

for config in "${CONFIGS[@]}"; do
    echo ""
    echo "##################################################"
    echo " CONFIG: $config"
    echo "##################################################"
    
    # Build the binary for this configuration if needed
    ./build-tsvc.sh "$config"
    BINARY="bin/GNU/tsvc_vec_$config"
    
    if [ ! -f "$BINARY" ]; then
        echo "Error: Binary $BINARY not found. Skipping config $config."
        continue
    fi

    for vlen in "${VLEN_OPTIONS[@]}"; do
        echo "--------------------------------------------------"
        echo " VLEN = $vlen"
        echo "--------------------------------------------------"
        
        for lanes in "${LANE_COUNTS[@]}"; do
            echo " Running LANES (Throughput) = $lanes, VLEN = $vlen, CONFIG = $config"
            
            for test in "${TESTS[@]}"; do
                for iters in "${ITER_COUNTS[@]}"; do
                    echo "  Processing $test (iters=$iters, lanes=$lanes, vlen=$vlen, config=$config)..."
                    
                    TEST_ITER_DIR="$RESULTS_DIR/$config/vlen_${vlen}/lanes_${lanes}/$test/iter_$iters"
                    mkdir -p "$TEST_ITER_DIR"
                    
                    START_TIME=$(date +%s.%N)
                    
                    # Execute run.sh with specific parameters
                    ITERATIONS=$iters LANES=$lanes VLEN=$vlen ./run.sh "$BINARY" "$test" 2>&1 | tee "$TEST_ITER_DIR/terminal.log"
                    
                    END_TIME=$(date +%s.%N)
                    
                    ELAPSED=$(python3 -c "print(round($END_TIME - $START_TIME, 3))" 2>/dev/null || echo "N/A")
                    echo "  Host Wallclock Time: $ELAPSED seconds" | tee -a "$TEST_ITER_DIR/terminal.log"
                    
                    # Save m5out directory
                    if [ -d "m5out" ]; then
                        mv m5out "$TEST_ITER_DIR/"
                        echo "  Saved results to $TEST_ITER_DIR/m5out"
                    else
                        echo "  Warning: m5out not found for $test"
                    fi
                done
            done
        done
    done
done

echo ""
echo "=================================================="
echo "Sweep execution completed. Results in: $RESULTS_DIR"
echo "=================================================="

# Generate summary CSV
if [ -f "./collect_subset_csv.py" ]; then
    echo "Generating summary CSV..."
    python3 ./collect_subset_csv.py "$RESULTS_DIR"
fi
