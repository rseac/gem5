#!/bin/bash

# List of tests to run
TESTS=(
    "s000"
    "s121"
    "s131"
    "s162"
    "s171"
    "s211"
    "s243"
    "s251"
    "s128"
    "s311"
    "s341"
)

# List of iteration counts to run
ITER_COUNTS=(1 5 10 20)

# List of lane counts (vector-timing-throughput) to run
LANE_COUNTS=(2 4)

# List of data configurations to run
CONFIGS=("TINY")

# Create a timestamped directory for results
TIMESTAMP=$(date +%Y%m%d_%H%M%S)
RESULTS_DIR="subset_results_$TIMESTAMP"
mkdir -p "$RESULTS_DIR"

echo "Starting subset runs with configs: ${CONFIGS[*]}"
echo "Lanes (throughput): ${LANE_COUNTS[*]}"
echo "Iterations: ${ITER_COUNTS[*]}"
echo "Tests: ${TESTS[*]}"
echo "Results folder: $RESULTS_DIR"
echo "--------------------------------------------------"

for config in "${CONFIGS[@]}"; do
    echo "##################################################"
    echo " CONFIG: $config"
    echo "##################################################"
    
    # Build the binary for this configuration if it doesn't exist or just to be sure
    ./build-tsvc.sh "$config"
    BINARY="bin/GNU/tsvc_vec_$config"
    
    if [ ! -f "$BINARY" ]; then
        echo "Error: Binary $BINARY not found. Skipping config $config."
        continue
    fi

    for lanes in "${LANE_COUNTS[@]}"; do
        echo "=================================================="
        echo "Running with LANES (Throughput) = $lanes"
        echo "=================================================="
        
        for test in "${TESTS[@]}"; do
            for iters in "${ITER_COUNTS[@]}"; do
                echo "Processing $test with $iters iterations (lanes=$lanes, config=$config)..."
                
                # Define a subdirectory for this config, lanes, test and iteration count
                TEST_ITER_DIR="$RESULTS_DIR/$config/lanes_$lanes/$test/iter_$iters"
                mkdir -p "$TEST_ITER_DIR"
                
                # Measure wallclock time using script timestamps
                START_TIME=$(date +%s.%N)
                
                # Run the test, passing iterations and lanes via environment variables
                ITERATIONS=$iters LANES=$lanes ./run.sh "$BINARY" "$test" 2>&1 | tee "$TEST_ITER_DIR/terminal.log"
                
                END_TIME=$(date +%s.%N)
                
                # Calculate elapsed time using python3 (since bc might not be present)
                ELAPSED=$(python3 -c "print(round($END_TIME - $START_TIME, 3))")
                echo "Host Wallclock Time: $ELAPSED seconds" | tee -a "$TEST_ITER_DIR/terminal.log"
                
                # Move the m5out directory produced by run.sh into the test's subdirectory
                if [ -d "m5out" ]; then
                    mv m5out "$TEST_ITER_DIR/"
                    echo "Saved results for $test (iters=$iters, lanes=$lanes, config=$config) to $TEST_ITER_DIR"
                else
                    echo "Warning: m5out not found for $test"
                fi
                echo "--------------------------------------------------"
            done
        done
    done
done

echo "All subset scaling runs completed. Results are in $RESULTS_DIR"

# Generate the summary CSV
if [ -f "./collect_subset_csv.py" ]; then
    echo "Generating summary CSV..."
    python3 ./collect_subset_csv.py "$RESULTS_DIR"
fi
