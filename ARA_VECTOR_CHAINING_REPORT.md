# ARA Vector Chaining Implementation: Technical Report

This document provides a definitive, line-by-line breakdown of the architectural changes made to the gem5 source code to support **Vector Chaining**, specifically tailored for the ARA hardware timing model.

---

## 1. Executive Summary

In baseline gem5, vector instructions occupy functional units (FUs) for their entire duration, and dependents must wait for full completion. In ARA hardware, consumers can start processing as soon as the producer's first elements clear the pipeline. 

Our implementation decouples **Structural Hazards** (FU occupancy) from **Data Hazards** (Result availability), accurately modeling the element-streaming nature of ARA.

### Key Logic Decoupling
- **`dynamicOpLatency`**: Models **FU Occupancy**. It represents the time the FU is physically busy processing elements.
- **`chainingLatency`**: Models **Result Readiness**. It represents the cycle when the *first* elements are written back and ready for consumption.

---

## 2. Hardware-Rooted Justification (ARA RTL)

Research into the ARA hardware source (`ara/hardware/src/lane/operand_requester.sv`) confirmed:
1.  **No Bypassing**: ARA does not have operand bypassing; results must be written to the Vector Register File (VRF) before being read.
2.  **Registered Synchronization**: The `operand_requester` uses a registered signal `vinsn_result_written_q`.
3.  **Issue-to-Issue Delay**: This adds a **2-cycle physical overhead** on top of the fixed pipeline depth (e.g., 5 cycles for FP).
4.  **Throughput scaling**: FU occupancy is strictly `Total_Bits / (Throughput_Factor * ELEN)`.

---

## 3. Exhaustive Code Modifications

### A. Core StaticInst Interface
**File: `gem5/src/cpu/static_inst.hh`**
Added the standard interface for all instructions to report their chaining capability.
```cpp
virtual Cycles
dynamicOpLatency(ThreadContext *tc) const {
    return Cycles(0);
}

virtual Cycles
chainingLatency(ThreadContext *tc) const {
    return dynamicOpLatency(tc); // Defaults to wait-until-end for scalars
}
```

### B. Global SimObject Parameters
**Files: `gem5/src/cpu/BaseCPU.py`, `base.hh`, `base.cc`**
```python
# BaseCPU.py
enable_vector_chaining = Param.Bool(True, "Enable RISC-V Vector Chaining")
vector_timing_throughput = Param.Unsigned(2, "Elements processed per cycle (timing model)")
```
```cpp
// base.hh & base.cc
bool enableVectorChaining;
unsigned vectorTimingThroughput; // Initialized in constructor
```

### C. RISC-V Vector Timing Model
**File: `gem5/src/arch/riscv/insts/vector.hh`**
Implemented the ARA-specific throughput and pipeline depth logic.

```cpp
Cycles
dynamicOpLatency(ThreadContext *tc) const override {
    const int NrLanes = tc->getCpuPtr()->vectorTimingThroughput;
    const int ELEN = 64;
    int elements_per_cycle = NrLanes * (ELEN / sew);
    if (elements_per_cycle == 0) elements_per_cycle = 1;

    // Occupancy = Throughput Cycles
    return Cycles((microVl + elements_per_cycle - 1) / elements_per_cycle);
}

Cycles
chainingLatency(ThreadContext *tc) const override {
    if (!tc->getCpuPtr()->enableVectorChaining) return dynamicOpLatency(tc);
    
    int pipeline_lat = ...; // (Mapping: FP=5, Int=1, Div=32)
    const int CHAINING_OVERHEAD = 2; // Matches ARA hardware synchronization
    return Cycles(pipeline_lat + CHAINING_OVERHEAD);
}
```

---

## 4. Stability Guards & Functional Logic (O3CPU)

To prevent SEGVs and LSQ assertions, the following decoupled logic was implemented in `inst_queue.cc`:

1.  **Functional Ordering**: The `WakeDependents` event (at `chainingLatency`) executes the producer functionally *before* waking consumers.
2.  **Redundancy Guard**: Added `isResultReady()` check to prevent multiple wakeups from the same instruction.
3.  **Memory Consistency**: Chaining is restricted to arithmetic operations to preserve LSQ ordering.

---

## 5. Configuration Guidelines

The relationship between `simd-units` and `vector-timing-throughput` is as follows:

1.  **`simd-units` (Hardware Lanes)**: This parameter in `riscv-rvv-se-ara.py` sets the number of physical functional units in the functional unit pool. This determines how many instructions can be issued in parallel (structural parallelism).
2.  **`vector-timing-throughput` (Timing Model)**: This parameter determines the cycle-by-cycle throughput used in the `dynamicOpLatency` calculation. This determines how fast an individual instruction finishes.

**Standard ARA Configuration**:
To model a standard 4-lane ARA engine:
- `--simd-units 4` (Models 4 physical lanes).
- `--vector-timing-throughput 4` (Models the throughput speedup of those lanes).

---

## 6. Usage Instructions

Compile gem5:
```bash
scons build/RISCV/gem5.opt -j$(nproc)
```

Run simulation:
```bash
./build/RISCV/gem5.opt rvv/riscv-rvv-se-ara.py <binary> \
    --cpu-type AraO3 \
    --enable-chaining \
    --simd-units 4 \
    --vector-timing-throughput 4 \
    --vlen 1024
```
