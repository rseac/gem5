#!/bin/bash

# generate_golden.sh
# Generates golden checksum files for a specific data configuration.

CONFIG=${1:-"TINY"}
ITERATIONS=(1 5 10 20)
TARGET_DIR="input-${CONFIG,,}" # lowercase config

# Ensure directory exists and is writable
mkdir -p "$TARGET_DIR"
chmod 777 "$TARGET_DIR"

# Function to run command in docker
run_in_docker() {
    docker exec -w /gem5/tsvc-gem5 nice_wu bash -c "$1"
}

echo "Generating golden checksums for CONFIG=$CONFIG (NATIVE inside docker)..."
# Build natively inside the container
run_in_docker "make -C src COMPILER=native CONFIG=$CONFIG NO_M5OPS=1 BIN_DIR=."

BINARY="src/tsvc_vec"

# Verify binary exists
if ! run_in_docker "[ -f $BINARY ]"; then
    echo "Error: Binary $BINARY not found in container."
    exit 1
fi

for i in "${ITERATIONS[@]}"; do
    GOLDEN_FILE="$TARGET_DIR/checksum.golden.iter$i"
    echo "  Processing iterations=$i -> $GOLDEN_FILE"
    
    # Header - Run directly in docker to handle file path correctly
    run_in_docker "echo 'Running all benchmarks' > $GOLDEN_FILE"
    run_in_docker "echo 'Loop 	Time(sec) 	Checksum' >> $GOLDEN_FILE"
    
    # Run and process inside docker. 
    # Use a single-quoted string for the inner command to prevent host shell expansion.
    docker exec -w /gem5/tsvc-gem5 nice_wu bash -c "./$BINARY -i $i | awk '
    /^[[:space:]]*(s[0-9]+|v[a-z]+)/ {
        if (\$2 == \"cycles:\" || \$2 == \"start\" || \$2 == \"end\") next;
        kernel = \$1;
        checksum = \$NF;
        printf \"risky  %-12s     0.000	%s\n\", kernel, checksum
    }' >> $GOLDEN_FILE"
    
    run_in_docker "echo 'Done' >> $GOLDEN_FILE"
done

# Cleanup
run_in_docker "rm $BINARY"

echo "--------------------------------------------------"
echo "Golden generation complete. Files are in $TARGET_DIR"
