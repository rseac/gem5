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
In the `MinorCPU`, chaining is implemented via **Scoreboard Decoupling**.

1.  **Dual-Timing Issue**: When an instruction is issued, it calculates:
    - **`inst_opLat`**: FU Occupancy (Total time busy).
    - **`inst_chainingLat`**: Result Ready time (Early wakeup).
2.  **Scoreboard Markup**: Destination registers are marked ready at `curCycle + inst_chainingLat`.
3.  **Overlapped Execution**: Pipelined vector units (`issueLat = 1`) allow consumers to issue while the producer is still in its occupancy phase.

**Note on MinorCPU Effectiveness**: Chaining benefits in `AraMinor` are most visible with large vectors (`VLEN >= 1024`). With smaller vectors, the functional unit occupancy often completes before the pipeline depth delay is finished, leaving no "tail" elements to overlap with the next instruction.

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
    - **Functional Order**: By executing the producer functionally at the chaining point, we ensure consumers do not read uninitialized data (fixing the "Address 0" SEGV).
    - **Double-Issue Guard**: A check in `processFUCompletion` ensures that if an instruction was already added to the execution list by the early `WakeDependents` event, it is not added again.

---

## 4. Exhaustive Code Modifications

### A. StaticInst Base Interface (`src/cpu/static_inst.hh`)
```cpp
virtual Cycles dynamicOpLatency(ThreadContext *tc) const;
virtual Cycles chainingLatency(ThreadContext *tc) const; // Defaults to dynamicOpLatency
```

### B. Global Parameters (`BaseCPU.py`, `base.hh`, `base.cc`)
```python
enable_vector_chaining = Param.Bool(True, "Enable RISC-V Vector Chaining")
vector_timing_throughput = Param.Unsigned(2, "Elements processed per cycle")
```
```cpp
bool enableVectorChaining;
unsigned vectorTimingThroughput; 
```

### C. Functional Unit Configuration (`AraConfig.py`, `AraMinorConfig.py`)
```python
simd_units = Param.Unsigned(4, "Number of SIMD functional units (physical lanes)")
# O3: Dynamically sets AraSIMD_Unit.count = simd_units
# Minor: Dynamically extends FU list with (simd_units - 1) extra vector units
```

---

## 5. Configuration Guidelines

1.  **`simd-units`**: Models the **Structural Parallelism**. Set this to match the number of physical lanes if you want to model instruction-level parallelism limitations.
2.  **`vector-timing-throughput`**: Models the **Data Parallelism**. Set this to match the lanes to see the throughput speedup of a single engine.

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
