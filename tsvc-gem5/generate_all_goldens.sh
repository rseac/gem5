#!/bin/bash

# Wrapper to generate golden checksums for standard configurations.
# This script should be run inside the docker container to avoid permission issues.

# Note: Only TINY and SMALL are recommended for gem5 simulation.
# MEDIUM/LARGE/HUGE will take excessive time (hours to days).
CONFIGS=("TINY" "SMALL" "MEDIUM")

for config in "${CONFIGS[@]}"; do
    echo "=================================================="
    echo " Processing CONFIG: $config"
    echo "=================================================="
    ./generate_golden.sh "$config"
done

echo "=================================================="
echo "Golden checksums generated for: ${CONFIGS[*]}"
echo "To generate larger configs, please use a native build or expect long simulation times."
