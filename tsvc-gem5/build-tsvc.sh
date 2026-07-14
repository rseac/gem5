#!/bin/bash

# Configuration settings
COMPILER="GNU"
CONFIG=${1:-"TINY"}
NO_OMP=1

echo "Building TSVC binaries for CONFIG=$CONFIG..."
echo "  Compiler: $COMPILER"
echo "  Config:   $CONFIG"
echo "  OpenMP:   $( [ $NO_OMP -eq 1 ] && echo "Disabled" || echo "Enabled" )"
echo "--------------------------------------------------"

# Run make in the src directory
make -C src COMPILER=$COMPILER CONFIG=$CONFIG NO_OMP=$NO_OMP

if [ $? -eq 0 ]; then
    echo "--------------------------------------------------"
    echo "Build successful! Renaming binaries with config suffix..."
    # Rename binaries to include the config name
    for f in bin/$COMPILER/tsvc_*; do
        if [[ $f == *"_TINY" || $f == *"_SMALL" || $f == *"_MEDIUM" || $f == *"_LARGE" || $f == *"_HUGE" ]]; then
            continue # Skip already renamed
        fi
        mv "$f" "${f}_${CONFIG}"
    done
    ls -lh bin/$COMPILER/tsvc_*_${CONFIG}
    
    # Generate golden reference input files for validation
    if [ -f "./generate_golden_container.sh" ]; then
        echo "--------------------------------------------------"
        echo "Generating golden reference checksums for $CONFIG..."
        ./generate_golden_container.sh "$CONFIG"
    fi
else
    echo "--------------------------------------------------"
    echo "Build failed. Please check the error messages above."
    exit 1
fi
