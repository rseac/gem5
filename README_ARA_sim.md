# Running gem5 with ARA Vector Latencies

This guide explains how to use the custom ARA (Automatic Router Architecture) vector latency model in gem5.

## Overview

We have implemented a custom configuration that replaces the default 1-cycle O3 CPU vector latencies with realistic values derived from the ARA hardware documentation (e.g., 5-cycle FP64 multiply, 32-cycle Integer Divide).

## Files Added

*   **Configuration:** `src/cpu/o3/AraConfig.py`
    *   Defines the `AraSIMD_Unit` and `AraFUPool`.
*   **Simulation Script:** `configs/example/ara_simulation.py`
    *   A ready-to-run script that uses the ARA configuration.

## Prerequisites

Ensure you have built the RISC-V version of gem5:

```bash
scons build/RISCV/gem5.opt -j$(nproc)
```

## How to Run

Use the provided simulation script `configs/example/ara_simulation.py`. You need to edit the script to point to your RISC-V binary or pass it as an argument if you modify the script to accept one.

### Command

You must pass the path to your RISC-V binary as an argument:

```bash
# Run from the gem5 root directory
build/RISCV/gem5.opt configs/example/ara_simulation.py /path/to/your/binary
```

Example:
```bash
build/RISCV/gem5.opt configs/example/ara_simulation.py tests/test-progs/hello/bin/riscv/linux/hello
```

## Output

The simulation will run and print the standard gem5 startup messages. At the end of the execution, it will print the total execution cycles calculated from the 1GHz clock frequency:

```text
Beginning simulation with ARA Latencies configuration!
CPU FUPool Class: AraFUPool
...
Exiting @ tick ...
Total Execution Cycles: 12345
```

## Modifying Latencies

If you need to adjust the latencies (e.g., to test optimization sensitivity):

1.  Open `src/cpu/o3/AraConfig.py`.
2.  Locate the `AraSIMD_Unit` class.
3.  Change the `opLat` value for the desired `OpDesc`.
4.  Re-run the simulation (no recompilation of gem5 is needed since it's a Python config change).
