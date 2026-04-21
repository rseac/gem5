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

### 3. VLSU Pipeline Calibration (Suggestion #3)
- **AGU Latency**: Increased `opLat` for all SIMD memory operations from 1 to **3 cycles** in `AraConfig.py`.

## Calibration Results

| Test | ARA RTL Cycles | Baseline gem5 | Calibrated gem5 (v1) | Calibrated gem5 (v2 - Thin O3) | Final Discrepancy |
| :--- | :--- | :--- | :--- | :--- | :--- |
| **s000** (Arithmetic) | 49,228 | 17,722 | 50,365 (1.02x) | **53,322** | **1.08x** |
| **vpv** (Mixed) | 402,682 | 79,008* | 255,940 (1.57x) | **291,126** | **1.38x** |
| **va** (Memory-Only) | 373,200 | 79,008 | 233,438 (1.6x) | **247,860** | **1.51x** |

*\*Note: Baseline vpv assumed similar to va.*

## Conclusion
The calibration has successfully closed the gap:
- **Arithmetic kernels** are within **8%** of the RTL.
- **Memory-influenced kernels** have improved from ~4.7x faster to ~1.4x-1.5x faster.
