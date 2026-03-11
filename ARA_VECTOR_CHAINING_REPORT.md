# ARA Vector Chaining Implementation: Technical Report

This document provides a definitive, line-by-line breakdown of the architectural changes made to the gem5 source code to support **Vector Chaining**, specifically tailored for the ARA hardware timing model.

---

## 1. Executive Summary

In baseline gem5, vector instructions occupy functional units (FUs) for their entire duration, and dependents must wait for full completion. In ARA hardware, consumers can start processing as soon as the producer's first elements clear the pipeline. 

Our implementation decouples **Structural Hazards** (FU occupancy) from **Data Hazards** (Result availability), accurately modeling the element-streaming nature of ARA.

### Key Logic Decoupling
- **`dynamicOpLatency`**: Models **FU Occupancy**. It represents the time the FU is physically busy processing elements (throughput).
- **`chainingLatency`**: Models **Result Readiness**. It represents the cycle when the *first* elements are written back and ready for consumption.

---

## 2. Hardware-Rooted Justification (ARA RTL)

Research into the ARA hardware source (`ara/hardware/src/lane/operand_requester.sv`) confirmed:
1.  **No Bypassing**: ARA does not have operand bypassing; results must be written to the Vector Register File (VRF) before being read.
2.  **Registered Synchronization**: The `operand_requester` uses a registered signal `vinsn_result_written_q`.
3.  **Issue-to-Issue Delay**: This adds a **2-cycle physical overhead** on top of the fixed pipeline depth (e.g., 5 cycles for FP).
4.  **Throughput scaling**: FU occupancy is strictly `Total_Bits / (Lanes * Data_Width_Per_Cycle)`.

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
Added parameters to scale the model for any ARA lane configuration.
```python
# BaseCPU.py
enable_vector_chaining = Param.Bool(True, "Enable RISC-V Vector Chaining")
vector_lanes = Param.Unsigned(2, "Number of vector lanes")
```
```cpp
// base.hh & base.cc
bool enableVectorChaining;
unsigned vectorLanes; // Initialized in constructor
```

### C. RISC-V Vector Timing Model
**File: `gem5/src/arch/riscv/insts/vector.hh`**
Implemented the ARA-specific throughput and pipeline depth logic.

**Note on Lane Configuration vs. Unit Count**:
- **`vector_lanes`**: Controls the internal datapath width of a single execution unit. It determines the throughput cycles (occupancy) of a vector instruction.
- **`AraSIMD_Unit.count`** (in `AraConfig.py`): Controls how many independent vector units the CPU has. This models superscalar capability (ILP).
- The model accurately separates these: `vector_lanes` determines how long one instruction keeps a unit busy, while `count` determines how many such units are available.

```cpp
Cycles
dynamicOpLatency(ThreadContext *tc) const override {
    const int NrLanes = tc->getCpuPtr()->vectorLanes;
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

### D. MinorCPU (In-Order) Integration
**File: `gem5/src/cpu/minor/execute.cc`**
Modified scoreboard markup to release registers early for chaining.
```cpp
Cycles inst_chainingLat = inst_opLat;
if (auto chain_lat = inst->staticInst->chainingLatency(cpu.getContext(thread_id));
    chain_lat > Cycles(0)) {
    inst_chainingLat = chain_lat;
}

// Scoreboard uses early chaining, while FU model uses full occupancy
scoreboard[thread_id].markupInstDests(inst, cpu.curCycle() + inst_chainingLat + ...);
```

### E. O3CPU (Out-of-Order) Integration
**File: `gem5/src/cpu/o3/inst_queue.hh` & `.cc`**
Introduced an event-driven split to handle early execution and result availability.

#### 1. Decoupled Events
- **`WakeDependents`**: A new event scheduled at `chainingLatency`.
- **`FUCompletion`**: Scheduled at the full `dynamicOpLatency`.

#### 2. Functional Ordering & Stability
To prevent SEGVs (Page Table Fault at Address 0) and LSQ assertions:
```cpp
void InstructionQueue::WakeDependents::process() {
    if (!inst->isSquashed()) {
        // MUST execute functionally so consumer reads valid data
        iqPtr->issueToExecuteQueue->access(-1)->size++;
        iqPtr->instsToExecute.push_back(inst);
        iqPtr->wakeDependents(inst);
    }
}

// wakeDependents Guard
int InstructionQueue::wakeDependents(const DynInstPtr &completed_inst) {
    if (completed_inst->isResultReady()) return 0; // Prevent redundant wakeups
    completed_inst->setResultReady();
    ...
}

// processFUCompletion Guard
if (chaining_latency < op_latency && !inst->isMemRef()) {
    return; // Already handled by WakeDependents
}
```

---

## 4. Verification & Performance Model

### Latency Example (LMUL=8, VLEN=1024, SEW=32, Lanes=2)
| Instruction | Logic | Result |
| :--- | :--- | :--- |
| **FU Occupancy** | `1024 bits / (2 lanes * 64 bits)` | **8 Cycles** |
| **Start-to-Ready** | `5 (Pipe) + 2 (Overhead)` | **7 Cycles** |

### Stability Features
- **Memory Consistency**: Chaining is restricted to arithmetic operations to preserve LSQ ordering invariants.
- **Squash Robustness**: All decoupled events verify the instruction's squash status before modifying state.
- **Dynamic Scaling**: The model supports 1, 2, 4, 8, or 16 lanes via the `vector_lanes` parameter.

---

## 5. Usage Instructions

Compile gem5 for RISC-V:
```bash
scons build/RISCV/gem5.opt -j$(nproc)
```

Run simulation with chaining control:
```bash
./build/RISCV/gem5.opt rvv/riscv-rvv-se-ara.py <binary> \
    --cpu-type AraO3 \
    --enable-chaining \
    --vector-lanes 4 \
    --vlen 1024
```
