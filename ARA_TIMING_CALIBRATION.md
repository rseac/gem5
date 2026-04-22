# ARA RTL to gem5 Timing Calibration

## Overview
This document summarizes the changes made to the gem5 `AraO3` timing model to better align its cycle counts with the ARA RTL hardware simulation (Verilator).

## Identified Discrepancies
Initial comparisons using the `s000` and `va` tests showed that gem5 was significantly more optimistic than the RTL, especially for memory-intensive workloads:
- **s000 (Arithmetic)**: gem5 was ~2.8x faster.
- **va (Memory Assignment)**: gem5 was ~4.7x faster.

The primary cause was identified as the gem5 memory system providing much higher bandwidth and lower latency than the AXI-interconnected SRAM/L2 system in the ARA SoC.

## Changes Implemented
The following adjustments were made in the `ara-timing-calibration` branch:

### 1. Memory Model Calibration (`riscv-rvv-se-ara.py`)
- **Memory Type**: Swapped `SingleChannelDDR4_2400` for `SingleChannelSimpleMemory` to emulate the low-latency but contention-prone SRAM main memory of the ARA RTL.
- **L1D Cache**:
    - Increased `tag_latency` and `data_latency` to **4 cycles** (accounting for VLSU pipeline + AXI interconnect).
    - Reduced `mshrs` to **2** (matching ARA's shallow request queues).
- **L2 Cache**:
    - Increased `tag_latency` and `data_latency` to **12 cycles** (accounting for AXI crossbar overhead).
    - Reduced `mshrs` to **4** (matching AXI transaction limits).
- **Command-line Overrides**: Added arguments (`--l1d-lat`, `--l1d-mshrs`, `--l2-lat`, `--l2-mshrs`, `--mem-lat`) to allow fine-tuning without script modification.

### 1. Memory Model Calibration (v5 - Final)
To match the 4-lane ARA RTL behavior, the gem5 memory system was further refined:
- **Bus Width**: Restricted `membus` and `L2Bus` width to **16 bytes** (128-bit) to match ARA's physical AXI data width.
- **Cache Latencies**: Increased L1D and L1I hit latencies to **8 cycles** to account for the full hardware pipeline round-trip through the AXI fabric.
- **MSHRs**: Reduced L2 MSHRs to **2** to limit concurrent AXI transactions.

### 2. Core Model Calibration (Suggestion #1 - "Thin" O3)
To mimic the single-issue CVA6 core used in ARA, the gem5 `AraO3` model uses restricted widths and buffer sizes. 

**Proportional Scaling**: To ensure the model scales correctly with wider lane configurations, all core resources scale with the lane count ($L$):
- **Scale Factor ($S$**) $= \max(1, L/2)$
- **Pipeline Widths** (Fetch, Decode, Commit, etc.) $= S$
- **ROB Size** $= 32 \times S$
- **IQ Size** $= 16 \times S$
- **LSQ Size** $= 16 \times S$
- **Physical Registers** $= 64 \times S$
- **Cache Ports**: Strictly set to **1** (Load) and **1** (Store) to match ARA's single-ported memory bottleneck regardless of core width.

### 3. VLSU and Dispatch Calibration
- **AGU Latency**: Increased to **3 cycles**.
- **Dispatch Floor**: Increased to **10 cycles**.

## Calibration Results (4-Lane Configuration)

| Test | ARA RTL Cycles | Calibrated gem5 (v8 - Prop. Scaling) | Final Discrepancy | Characteristic |
| :--- | :--- | :--- | :--- | :--- |
| **va** | 298,760 | **301,665** | **0.99x (Perfect)** | Pure Unit-Stride |
| **vpv** | 323,657 | **321,205** | **1.01x (Perfect)** | Mixed Unit-Stride |
| **vdotr** | 188,601 | 321,326 | **0.59x** | Reduction (v6 Optimized) |
| **s1111** | 71,572 | 189,524 | **0.37x** | Strided Memory (v6 Optimized) |
| **s000** | 38,591 | 64,570 | **0.60x** | Pure Arithmetic |

## Calibration Results (8-Lane Configuration)

| Test | ARA RTL Cycles | Calibrated gem5 (v8) | Discrepancy | Characteristic |
| :--- | :--- | :--- | :--- | :--- |
| **s000** | 34,811 | 78,924 | **0.44x** | Pure Arithmetic |
| **va** | *Running* | 414,233 | - | Pure Unit-Stride |
| **vpv** | *Running* | 447,212 | - | Mixed Unit-Stride |

## Scaling Observations
1.  **Memory Accuracy**: The model achieved near-perfect accuracy (within 1%) for the 4-lane unit-stride tests, validating the memory system calibration.
2.  **Arithmetic Scaling Challenge**: As the number of lanes increases (2 -> 4 -> 8), the arithmetic discrepancy grows (0.92x -> 0.60x -> 0.44x). Even with proportional resource scaling, gem5's Out-of-Order model becomes significantly slower than the RTL for compute-bound loops. This suggests that the internal instruction dispatch and sequencer overhead in ARA is much lower than the current gem5 O3 model can mimic for wide vector units.
