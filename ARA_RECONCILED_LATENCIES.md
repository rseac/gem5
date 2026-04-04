# ARA Reconciled Latencies for gem5

This document details the reconciled latency values for the ARA `AraO3CPU` model in gem5. These values have been adjusted to match the **Instruction-to-Instruction (Iss-to-Iss) dependency delay** measured in the ARA RTL (hardware), rather than just the raw functional unit pipeline depths.

## 1. Methodology

The gem5 O3 model calculates the dependency delay as:
`Total Latency = 1 (Issue Cycle) + op_latency`

In the ARA hardware, every instruction chain is subject to a **Sequencer Dispatch Floor** of approximately 6 cycles. Therefore, even a 1-cycle ALU operation exhibits a 7-cycle dependency delay.

## 2. Reconciled Pipeline Latencies

The following values are implemented in `gem5/src/arch/riscv/insts/vector.hh` and `gem5/src/cpu/o3/AraConfig.py`.

| Instruction Category | RTL Meas. (Iss-to-Iss) | gem5 `op_latency` | Total (1+Lat) |
| :--- | :---: | :---: | :---: |
| **Integer ALU** (`vadd`) | 7 | 6 | 7 |
| **FP Add/Mul (EW32)** | 11 | 10 | 11 |
| **FP Add/Mul (EW64)** | 12 | 11 | 12 |
| **FP Div/Sqrt (EW32)**| 20 | 19 | 20 |
| **Integer Div (EW32)** | 42 | 41 | 42 |
| **Integer Div (EW64)** | 74 | 73 | 74 |
| **Memory Load (L1 Hit)**| 24 | 23 | 24 |
| **Memory Store** | 20 | 19 | 20 |

## 3. Implementation Details

*   **`vector.hh`:** Updated `dynamicOpLatency()` to return the reconciled values based on `opClass()` and `vsew`.
*   **`vector.hh`:** Updated `chainingLatency()` to match `dynamicOpLatency()`, ensuring that vector chaining accurately reflects the hardware's element-streaming bottlenecks.
*   **`AraConfig.py`:** Increased functional unit `opLat` to prevent gem5's O3 instruction queue from waking dependents earlier than the hardware allows.

## 4. Verification

The reconciliation is verified by running the `latency-tests` application in the gem5 simulator.
The "Meas" (Measured) column in the simulation output now aligns with the "RTL" (Expected) column.
