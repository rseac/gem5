# ARA Latency Update — Reconciliation Report

This document describes the updates made to the gem5 `AraO3CPU` model to reconcile its simulation performance with the measurements from the ARA RTL (hardware).

## 1. Overview of Changes

The previous gem5 configuration used raw RTL pipeline depths (`opLat`), which failed to account for system-level overheads (sequencer bottleneck, iterative compute cycles) present in the hardware.

| Instruction Category | Before (`opLat`) | After (`opLat` / `chaining`) | Reason for Change |
| :--- | :---: | :---: | :--- |
| **Integer ALU** | 1 | 1 (Floor 7) | Aligns with 6-cycle hardware dispatch floor + 1 issue cycle. |
| **FP Arithmetic** | 4 | 4 (Overhead 6) | Accounts for higher synchronization delay in the MFPU pipeline. |
| **FP Division** | 3 | 20 | Models iterative SRT compute time instead of just pipeline registers. |
| **Integer Division**| 32 | 64 | Models worst-case serial bit-extraction for 64-bit elements. |

## 2. Technical Rationale

### 2.1 The Dispatch Floor (Sequencer Overhead)
The ARA lane sequencer has a measured dispatch bottleneck. Even if a functional unit is combinational, the system requires ~6 cycles to manage hazard checking and operand fetching.
*   **Update:** `DISPATCH_FLOOR` increased from 6 to 7 in `vector.hh` (includes 1 cycle for micro-op issue).

### 2.2 Iterative Unit Modeling
FP Division and Square Root are **non-pipelined** in ARA. The previous model used `opLat=3`, which only represented the interface registers.
*   **Update:** `opLat` increased to 20 in `AraConfig.py`.
*   **Update:** `pipeline_lat` in `vector.hh` increased to 17 to ensure chaining consumers wait for the full calculation.

### 2.3 Floating-Point Chaining Overhead
The MFPU (floating-point unit) result path is longer than the Integer ALU path in hardware, requiring more cycles to clear hazard synchronization.
*   **Update:** `fu_overhead` for MFPU classes increased from 2 to 6 in `vector.hh`.

## 3. Impact on Simulation
With these changes, gem5 "Meas" values now match the RTL "Meas" values for dependency chains:
*   **vadd (e32):** 7 cycles (exact match).
*   **vfadd (e32):** 11 cycles (exact match: 4 pipe + 6 overhead + 1 issue).
*   **vfdiv (e32):** 20 cycles (exact match).
