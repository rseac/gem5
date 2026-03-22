# ARA RTL Hardware Latency Verification Plan

## 1. Objective
Design and execute a suite of micro-benchmarks directly on the ARA hardware (RTL simulation or FPGA) to measure and confirm the exact latencies of various vector instructions. This establishes a ground-truth baseline to ensure the gem5 `AraO3` timing model is perfectly calibrated.

## 2. Test Suite Design

The tests must be written to isolate vector execution time from scalar overhead.

### A. Pipeline Depth (Dependent Chains)
- **Logic**: Create tight loops with strict data dependencies (e.g., `v1 = vadd(v1, v2)`, followed by `v1 = vmul(v1, v3)`).
- **Goal**: By forcing the hardware to wait for the chaining signal, we can deduce the exact pipeline depth plus chaining overhead.
- **Variants**: Test across all element widths (`SEW` = 8, 16, 32, 64) for Floating Point operations.

### B. Functional Unit Occupancy (Throughput)
- **Logic**: Execute a long sequence of independent vector operations.
- **Goal**: Measure the maximum throughput (elements per cycle) to confirm lane width capabilities.

### C. Non-Pipelined Blocking (Division)
- **Logic**: Sequence of independent `vdiv` or `vfdiv` operations.
- **Goal**: Confirm that the division unit does not accept new instructions until the current one finishes.

## 3. Measurement Mechanism
To get precise measurements on the ARA RTL without gem5's `m5ops`:
- Wrap the core execution loops with reads from the RISC-V Hardware Performance Counters (`mcycle` and `minstret`).
- Ensure loops are sufficiently unrolled.

## 4. Execution Workflow (ARA RTL)
1. **Compilation**: Compile the C/assembly tests using the RISC-V GNU Toolchain (`riscv64-unknown-elf-gcc`) with vector extensions enabled (`-march=rv64gcv`).
2. **RTL Simulation**: Boot the compiled binaries on the ARA RTL environment.
3. **Data Collection**: Extract the cycle counts from the UART output or simulation logs.

## 5. Validation and gem5 Calibration
- Compare empirical results against `ara_lmul1_latencies.md`.
- Update gem5 `AraO3` configurations if any RTL discrepancies are found.