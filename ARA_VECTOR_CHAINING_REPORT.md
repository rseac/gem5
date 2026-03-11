# ARA Vector Chaining Implementation: Technical Report

This document provides a definitive, line-by-line breakdown of the architectural changes made to the gem5 source code to support **Vector Chaining**, specifically tailored for the ARA hardware timing model.

---

## 1. Executive Summary

In baseline gem5, vector instructions occupy functional units (FUs) for their entire duration, and dependents must wait for full completion. In ARA hardware, consumers can start processing as soon as the producer's first elements clear the pipeline. 

Our implementation decouples **Structural Hazards** (FU occupancy) from **Data Hazards** (Result availability), accurately modeling the element-streaming nature of ARA.

---

## 2. Hardware-Rooted Justification (ARA RTL)

Research into the ARA hardware source (`ara/hardware/src/lane/operand_requester.sv`) confirmed:
1.  **No Bypassing**: ARA does not have operand bypassing; results must be written to the Vector Register File (VRF) before being read.
2.  **Registered Synchronization**: The `operand_requester` uses a registered signal `vinsn_result_written_q` to track written results.
3.  **Issue-to-Issue Delay**: This adds a **2-cycle physical overhead** on top of the fixed pipeline depth (e.g., 5 cycles for FP).
4.  **Throughput scaling**: FU occupancy is strictly the time to process elements through the physical lanes.

---

## 3. Timing Model Deep-Dive

### A. Core Vector Timing Logic (`vector.hh`)
The timing model uses the following variables to calculate latency:
- **`microVl`**: Number of elements in the current micro-op.
- **`vector_timing_throughput`**: The number of 64-bit element slots processed per cycle.
- **`sew`**: Standard Element Width (8, 16, 32, or 64).

#### Throughput Calculation Example:
**Configuration**: VLEN=1024, SEW=32, LMUL=8, `vector_timing_throughput`=2.
1.  **Elements per micro-op**: Each micro-op handles `VLEN/SEW` elements. $1024 / 32 = 32$ elements.
2.  **Elements per cycle**: The unit processes `throughput * (64/SEW)` elements per cycle. $2 \times (64/32) = 4$ elements/cycle.
3.  **Occupancy (`dynamicOpLatency`)**: $\text{PipelineDepth (5)} + (\text{Elements} / \text{ElementsPerCycle}) - 1 = 5 + (32/4) - 1 = \mathbf{12 \text{ Cycles}}$.
4.  **Readiness (`chainingLatency`)**: $\text{PipelineDepth (5)} + \text{Overhead (2)} = \mathbf{7 \text{ Cycles}}$.

---

### B. AraMinor (In-Order) Chaining Mechanism
In the `MinorCPU`, chaining is implemented via **Scoreboard Decoupling** and **Dynamic FU Occupancy**.

1.  **Dual-Timing Issue**: When an instruction is issued in `execute.cc`, the model calculates two distinct cycles:
    - **`inst_opLat`**: The total cycles the Functional Unit is busy.
    - **`inst_chainingLat`**: The cycle the result is ready for consumers.
2.  **Scoreboard Markup**: The destination registers are marked as ready at `curCycle + inst_chainingLat`. This allows dependent instructions to clear data hazards early.
3.  **Dynamic FU Occupancy**:
    - We added `overrideIssueLat` to the `QueuedInst` class in `func_unit.hh`.
    - Modified `FUPipeline::advance` in `func_unit.cc` to use this override when calculating `nextInsertCycle`.
    - This ensures that while consumers can clear data hazards early, the Functional Unit remains busy for the full `inst_opLat` (throughput duration), correctly modeling structural bottlenecks.
4.  **Functional Unit Pipelining**: We updated `AraMinorConfig.py` to ensure all vector units are configured as pipelined, allowing a consumer to issue while the producer is still in its occupancy phase.

---

### C. AraO3 (Out-of-Order) Chaining Mechanism
In the `O3CPU`, chaining is implemented via **Event-Driven Decoupling**.

1.  **Decoupled Scheduling**: Instead of a single completion event, the `InstructionQueue` now schedules two separate events:
    - **`WakeDependents` Event**: Scheduled at `chainingLatency`.
    - **`FUCompletion` Event**: Scheduled at `dynamicOpLatency`.
2.  **Early Functional Execution**:
    - The `WakeDependents` event is responsible for the **Functional Execution** of the instruction.
    - It adds the producer to the execution list so it actually generates data.
    - It then calls `wakeDependents()` to allow instructions in the Issue Queue to become "Ready".
3.  **Structural Integrity**:
    - The `FUCompletion` event handles the structural cleanup.
    - It releases the Functional Unit back to the `FUPool`.
    - It marks the producer as "Executed" for the commit stage.
4.  **Stability Guards**:
    - **Functional Order**: By executing the producer functionally at the chaining point, we ensure consumers do not read uninitialized data.
    - **Double-Issue Guard**: A check in `processFUCompletion` ensures that if an instruction was already added to the execution list by the early `WakeDependents` event, it is not added again.

---

## 4. Exhaustive Code Modifications

### A. Core StaticInst Interface (`src/cpu/static_inst.hh`)
```cpp
virtual Cycles dynamicOpLatency(ThreadContext *tc) const;
virtual Cycles chainingLatency(ThreadContext *tc) const; // Defaults to dynamicOpLatency
```

### B. Global Parameters (`BaseCPU.py`, `base.hh`, `base.cc`)
```python
enable_vector_chaining = Param.Bool(True, "Enable RISC-V Vector Chaining")
vector_timing_throughput = Param.Unsigned(2, "Elements processed per cycle")
simd_units = Param.Unsigned(1, "Number of physical SIMD lanes")
```

### C. MinorCPU Infrastructure (`func_unit.hh`, `func_unit.cc`, `execute.cc`)
```cpp
// Added overrideIssueLat to QueuedInst to allow dynamic FU occupancy
class QueuedInst {
    Cycles overrideIssueLat{0};
};

// Modified FUPipeline to respect the override
void FUPipeline::advance() {
    if (pushWire->overrideIssueLat > Cycles(0)) 
        nextInsertCycle = timeSource.curCycle() + pushWire->overrideIssueLat;
}
```

---

## 5. Configuration Guidelines

1.  **`simd-units`**: Models the **Structural Parallelism**. Match this to physical lanes to model ILP limitations.
2.  **`vector-timing-throughput`**: Models the **Data Parallelism**. Match this to lanes to see the throughput speedup of a single engine.

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
