# ARA Vector Chaining: Technical Guide for `AraMinor`

This document provides a detailed explanation of how vector chaining is implemented in the gem5 `MinorCPU` (in-order) model, specifically tailored for the ARA RISC-V architecture.

---

## 1. High-Level Concept: Decoupling Timing
In a standard in-order processor, an instruction occupies a Functional Unit (FU) and blocks dependent instructions in the Scoreboard until it is completely finished. For long vector instructions, this creates a massive performance bottleneck.

The ARA implementation decouples these two concepts:
1.  **Occupancy (`dynamicOpLatency`)**: How long the Functional Unit is physically busy.
2.  **Readiness (`chainingLatency`)**: When the *first* elements are ready for consumption by dependent instructions.

---

## 2. Step-by-Step Code Walkthrough

### Step 1: Adding the Occupancy Override
*File: `src/cpu/minor/func_unit.hh`*

We added a new field to the `QueuedInst` class, which represents an instruction moving through the FU pipeline. This allows each individual vector instruction to specify its own hardware occupancy.

```cpp
// Added to QueuedInst class
Cycles overrideIssueLat; // Allows an instruction to say "I need the FU for X cycles"
```

### Step 2: Enforcing Occupancy in the FU Pipeline
*File: `src/cpu/minor/func_unit.cc`*

We modified the `FUPipeline::advance` logic. Instead of using a fixed configuration value, the pipeline now checks if the instruction has a custom occupancy requirement.

```cpp
if (pushWire->overrideIssueLat > Cycles(0)) {
    // Block the FU until the full vector throughput duration has passed.
    nextInsertCycle = timeSource.curCycle() + pushWire->overrideIssueLat;
}
```

### Step 3: Calculating Two Latencies at Issue
*File: `src/cpu/minor/execute.cc`*

When a vector instruction is issued to the Execute stage, the model calculates both the total occupancy and the early readiness (chaining) latency.

```cpp
// Inside MinorCPU::Execute::issue
Cycles inst_opLat = inst->staticInst->dynamicOpLatency(tc);
Cycles inst_chainingLat = inst->staticInst->chainingLatency(tc);

// Tell the FU to be busy for the total duration (e.g., 32 cycles)
fu_inst.overrideIssueLat = inst_opLat; 
```

### Step 4: Early Scoreboard Release (The Chaining Trigger)
*File: `src/cpu/minor/execute.cc`*

This is the most critical change. Normally, the Scoreboard marks a destination register as "Busy" until the end of the instruction's total latency. For ARA, we change this to the **shorter** `chainingLatency`.

```cpp
// Release the register lock early so consumers can issue to OTHER units
scoreboard[thread_id].markupInstDests(
    inst,
    cpu.curCycle() + inst_chainingLat + extra_lat, // Use chainingLatency (e.g., 7)
    cpu.getContext(thread_id),
    ...
);
```

---

## 3. Illustrative Example

Consider two dependent instructions:
1. **Inst A (`VADD`)**: 32 elements.
2. **Inst B (`VMUL`)**: Depends on `VADD`.

### The Timeline with ARA Chaining:
- **T=0**: `VADD` issues to **FU #1**.
  - **FU #1** is locked for **32 cycles** (Occupancy).
  - **Scoreboard** marks the result register as "Busy" for only **7 cycles** (Readiness).
- **T=7**: The Scoreboard marks the register as **Ready**.
- **T=8**: `VMUL` sees the data is ready. Since **FU #2** is free, it issues and starts executing.
- **T=32**: `VADD` finally releases **FU #1**.
- **T=40**: `VMUL` completes its execution.

### Comparison:
| Model | Total Time | Logic |
| :--- | :--- | :--- |
| **Standard gem5** | **~75 Cycles** | Inst B waits for Inst A to finish entirely. |
| **ARA Chaining** | **~40 Cycles** | Inst B starts as soon as Inst A's first elements are ready. |

---

## 4. Why not use Micro-ops (like ARM SVE2)?
While ARM SVE2 often uses micro-ops to split macro-instructions, doing so at the **element level** for large vectors (e.g., 512 elements) would overwhelm the CPU's internal buffers (ROB/IQ). 

The `chainingLatency` approach provides the **exact same timing accuracy** as element-level micro-ops but maintains high simulation performance by keeping the instruction as a single unit in the CPU pipeline.
