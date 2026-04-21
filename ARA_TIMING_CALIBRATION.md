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

### 2. Core Model Calibration (Suggestion #1 - "Thin" O3)
To mimic the single-issue CVA6 core used in ARA, the gem5 `AraO3` model was restricted:
- **Pipeline Widths**: `fetchWidth`, `decodeWidth`, `renameWidth`, `dispatchWidth`, `issueWidth`, `commitWidth` all reduced to **1**.
- **Buffer Sizes**: `numROBEntries` reduced to **32**, `numEntries` (IQ) reduced to **16**.
- **Physical Registers**: Reduced to **64** (Int/Float).
- **Memory Queues**: `LQEntries` and `SQEntries` reduced to **8**.
- **Cache Ports**: `cacheLoadPorts` and `cacheStorePorts` reduced to **1**.

### 3. VLSU and Dispatch Calibration (Suggestions #3, #4, #5)
- **AGU Latency**: Increased `opLat` for all SIMD memory operations from 1 to **3 cycles**.
- **Dispatch Floor**: Increased `dispatchFloor` in `AraLatencyModel` from 6 to **10 cycles** to account for CVA6-to-ARA handshake overhead.

## Calibration Results

| Test | ARA RTL Cycles | Baseline gem5 | Calibrated gem5 (v3 - Thin O3/LSQ) | Calibrated gem5 (v4 - Serial/Floor) | Final Discrepancy |
| :--- | :--- | :--- | :--- | :--- | :--- |
| **s000** | 49,228 | 17,722 | 53,322 (1.08x) | **53,405** | **1.08x** |
| **vpv** | 402,682 | ~79,000 | 291,528 (1.38x) | **291,553** | **1.38x** |
| **va** | 373,200 | 79,008 | 248,225 (1.50x) | **247,261** | **1.51x** |

## Conclusion
The calibration has successfully closed the gap:
- **Arithmetic kernels** are within **8%** of the RTL.
- **Memory-influenced kernels** have improved from ~5x faster to **~1.4x-1.5x faster**.
- Remaining discrepancy is likely due to lack of AXI bus contention modeling between instruction and data fetches.
