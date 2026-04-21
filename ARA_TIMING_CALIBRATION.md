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
- **Pipeline Widths**: Reduced to **1** to mimic single-issue CVA6.
- **Buffer Sizes**: `ROB=32`, `IQ=16`, `LQ=8`, `SQ=8`.
- **Physical Registers**: Reduced to **64**.

### 3. VLSU and Dispatch Calibration
- **AGU Latency**: Increased to **3 cycles**.
- **Dispatch Floor**: Increased to **10 cycles**.

## Calibration Results (4-Lane Configuration)

| Test | ARA RTL Cycles | Calibrated gem5 | Final Discrepancy | Characteristic |
| :--- | :--- | :--- | :--- | :--- |
| **va** | 298,760 | 292,374 | **1.02x** | Pure Unit-Stride (Near Perfect) |
| **vpv** | 323,657 | 321,205 | **1.01x** | Mixed Unit-Stride (Near Perfect) |
| **vdotr** | 188,601 | 321,372 | **0.59x** | Reduction (gem5 is slower) |
| **s1111** | 71,572 | 248,587 | **0.29x** | Strided Memory (gem5 is slower) |
| **s000** | 38,591 | 50,612 | **0.76x** | Pure Arithmetic (gem5 is slower) |

## Conclusion
The model is now **perfectly calibrated for unit-stride vector processing** (the most common pattern in ARA). For specialized operations:
- **Reductions & Strided Memory**: Gem5 is significantly more pessimistic than the physical RTL, likely due to more complex micro-op decomposition in the gem5 C++ model.
- **Front-end Bottleneck**: In 4-lane configurations, the single-issue core width (`Width=1`) becomes a bottleneck for compute-bound kernels like `s000`.
