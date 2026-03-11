# Vector Chaining Implementation in gem5 ARA Timing Model

This document details the architectural changes made to gem5's timing models (MinorCPU and O3CPU) to support **Vector Chaining**, specifically aligned with the ARA hardware implementation.

## Overview

In the baseline gem5 model, a vector instruction occupies its functional unit (FU) for its entire duration, and dependent instructions can only issue after the producer has completely finished all its elements. In real ARA hardware, a consumer instruction can start processing its first elements as soon as the producer's first elements have cleared the execution pipeline.

Our implementation decouples the **Functional Unit Occupancy** (structural hazard) from the **Result Readiness** (data hazard), allowing for significant performance speedups in vector dependency chains.

---

## Architectural Changes

### 1. Core Interface Extension (`src/cpu/static_inst.hh`)
- Added `virtual Cycles chainingLatency(ThreadContext *tc)`.
- Defaults to `dynamicOpLatency()` for standard instructions.
- Provides a hook for the CPU models to query when the *first* results of an instruction are ready.

### 2. RISC-V Vector Timing Model (`src/arch/riscv/insts/vector.hh`)
Implemented dynamic latency calculations in `VectorMicroInst` based on ARA specifications:
- **`dynamicOpLatency`**: Returns the total cycles the instruction occupies the FU.
  - Formula: `Pipeline_Depth + (Total_Elements / Elements_Per_Cycle) - 1`
  - Elements per cycle is calculated dynamically using `vector_lanes` and `SEW`.
- **`chainingLatency`**: Returns the cycle when the first chunk of elements is ready.
  - Formula: `Pipeline_Depth + Chaining_Overhead (1 cycle)`
  - This models the physical delay of VRF write-back and hazard synchronization.

### 3. MinorCPU Scoreboard Integration (`src/cpu/minor/execute.cc`)
- Modified the issue logic to use `chainingLatency()` when marking destination registers in the scoreboard.
- This allows the in-order pipeline to issue dependent instructions earlier, while the producer is still processing remaining elements in the FU.

### 4. O3CPU Instruction Queue Refinement (`src/cpu/o3/inst_queue.cc`)
This was the most complex change, requiring a decoupling of events:
- **`WakeDependents` Event**: A new event scheduled at `chainingLatency`. It triggers:
  1. **Functional Execution**: The instruction generates its results so they are available in the physical registers.
  2. **Dependent Wakeup**: It calls `wakeDependents()` to notify instructions waiting in the IQ.
- **`FUCompletion` Event**: Remains scheduled at the full `dynamicOpLatency`. It handles:
  1. **FU Release**: Frees the functional unit for the next instruction.
  2. **Instruction Cleanup**: Marks the instruction as ready for commit.
- **Stability Guards**:
  - **Memory Ops**: Early chaining is disabled for memory instructions to prevent LSQ double-issue assertions.
  - **Squash Checks**: All events verify `!isSquashed()` before acting.
  - **Redundancy Guard**: Uses the `isResultReady()` flag to ensure `wakeDependents` and functional execution only happen once.

---

## Summary of New Features

| Feature | Description |
| :--- | :--- |
| **Element-Level Chaining** | Consumers start after the producer's pipeline depth, not after the full vector length. |
| **Dynamic Throughput** | Latencies scale automatically based on `vector_lanes`, `VLEN`, and `SEW`. |
| **Structural Integrity** | Functional units remain "busy" for the full duration, correctly modeling throughput bottlenecks. |
| **Runtime Toggle** | Enable or disable chaining via `--enable-chaining` or `--disable-chaining` flags. |
| **Lane Scaling** | The model supports any ARA lane configuration (1, 2, 4, 8, or 16 lanes) via the `vector_lanes` parameter. |

---

## Verification

### Performance Comparison
A dedicated benchmark `rvv_chaining_test.c` was created to measure the impact. For a long dependency chain (`vfadd` -> `vfmul`) with `LMUL=8`:
- **Chaining Disabled**: Total ticks are dominated by the sum of full vector latencies.
- **Chaining Enabled**: Total ticks are significantly reduced as the `vfmul` throughput overlaps with the `vfadd` throughput.

### Correctness
The `scalar_wakeup_test.c` verifies that operations requiring a complete vector result (like reductions feeding into a scalar move) still wait for the full `dynamicOpLatency`, ensuring architectural integrity.

---

## How to use
Add the following flags to your gem5 execution command:
- `--enable-chaining`: (Default) Enables early dependent wakeup.
- `--disable-chaining`: Forces consumers to wait for producer completion.
- `--vlen <bits>`: Adjusts vector length (impacts throughput).
- `--vector-lanes <count>`: Models different ARA hardware widths.
