# ARA Vector Chaining Implementation: Technical Report

This document provides a definitive technical breakdown of the vector chaining implementation in gem5, specifically modeled for the ARA hardware architecture.

---

## 1. High-Level Architectural Design

The primary goal is to accurately model ARA's **element-streaming** capability. In baseline gem5, vector instructions are monolithic; they occupy a Functional Unit (FU) and block dependents until the *entire* vector is finished. In ARA, as soon as the first elements clear the pipeline, they are written back and can be consumed by the next instruction.

### Logic Decoupling
Our implementation separates the hardware's behavior into two distinct timing signals:
1.  **`dynamicOpLatency` (Occupancy)**: How long the Functional Unit is physically busy. This determines **Structural Hazards**.
2.  **`chainingLatency` (Result Readiness)**: When the *first elements* are ready for consumption. This determines **Data Hazards**.

---

## 2. Shared Infrastructure (BaseCPU & RISC-V ISA)

These changes provide the foundation for both `AraMinor` and `AraO3`.

### A. Core Interface (`src/cpu/static_inst.hh`)
We added a new virtual method to the `StaticInst` base class to allow the CPU to query for chaining readiness.
```cpp
virtual Cycles dynamicOpLatency(ThreadContext *tc) const;
virtual Cycles chainingLatency(ThreadContext *tc) const; // Defaults to dynamicOpLatency
```

### B. Dynamic Latency Model (`src/arch/riscv/insts/vector.hh`)
Implemented the math for throughput scaling and hardware pipeline depths.
- **Throughput**: Calculated dynamically based on `NrLanes * (ELEN / SEW)`.
- **Dynamic Chaining Latency**: Calculated based on the Standard Element Width (SEW) to match ARA hardware:
  - **MFpu Ops**: `vsew + 2` (EW64=5, EW32=4, EW16=3, EW8=2).
  - **Integer Div**: `4 << vsew`.
  - **Integer Mult**: 0 cycles for EW8, 1 cycle otherwise.
- **Chaining Overhead**: A constant **2-cycle** synchronization overhead is added to all results.

```cpp
// Logic for occupancy (Occupancy = Throughput + Pipe if non-pipelined)
Cycles dynamicOpLatency(ThreadContext *tc) const {
    int elements_per_cycle = NrLanes * (ELEN / sew);
    int throughput = (microVl + elements_per_cycle - 1) / elements_per_cycle;
    int pipe = (isNonPipelined) ? (4 << vsew) : 0;
    return Cycles(pipe + throughput);
}

// Logic for readiness (When consumers can start)
Cycles chainingLatency(ThreadContext *tc) const {
    int pipeline_lat = vsew + 2; // MFpu scaling
    return Cycles(pipeline_lat + 2); // Pipe + ARA Overhead
}
```

---

## 3. Model Accuracy: AraMinor vs. AraO3

While both models implement chaining, **AraO3 (Out-of-Order)** is significantly more accurate for modeling high-performance vector engines like ARA.

| Feature | AraMinor (In-Order) | AraO3 (Out-of-Order) | Why AraO3 is More Accurate |
| :--- | :--- | :--- | :--- |
| **Instruction Issue** | Strict In-Order | Data-Flow (OoO) | AraO3 can issue independent math while waiting for a vector load. |
| **Independent Chains** | Serialized | Parallel | AraO3 executes multiple independent vector chains simultaneously. |
| **Chaining Trigger** | Scoreboard Release | Event-Based Handshake | AraO3 mimics hardware Valid/Ready signals via the `WakeDependents` event. |
| **Memory Timing** | Simple / Blocking | Detailed LSQ | AraO3 models speculative execution and store-to-load forwarding. |

**Recommendation**: Use **AraO3** for definitive performance analysis. **AraMinor** is useful for modeling the scalar core (CVA6) but underestimates the vector engine's throughput.

---

## 4. AraMinor (In-Order) Timing Model

### How it Works
In the `MinorCPU`, chaining works by releasing the **Scoreboard lock** early while keeping the **Functional Unit pipeline** busy.

### Key Code Changes
1.  **`src/cpu/minor/func_unit.hh`**: Added `overrideIssueLat` to the `QueuedInst` class to allow individual instructions to control FU occupancy.
2.  **`src/cpu/minor/func_unit.cc`**: Modified `FUPipeline::advance` to respect this override.
    ```cpp
    if (pushWire->overrideIssueLat > Cycles(0)) {
        nextInsertCycle = timeSource.curCycle() + pushWire->overrideIssueLat;
    }
    ```
3.  **`src/cpu/minor/execute.cc`**: Updated the issue logic to set the occupancy override and release the scoreboard early.
    ```cpp
    // Release Scoreboard at chainingLatency
    scoreboard[thread_id].markupInstDests(inst, cpu.curCycle() + inst_chainingLat + ...);
    
    // Occupy FU for dynamicOpLatency (Throughput)
    fu_inst.overrideIssueLat = inst_opLat;
    ```

### Example Calculation (`AraMinor`)
**Config**: VLEN=2048, SEW=32, Throughput=1, SIMD-Units=2.
- **`dynamicOpLatency`**: $2048 / (1 \times 64/32) = \mathbf{32 \text{ Cycles}}$. (FU is busy for 32 cycles).
- **`chainingLatency`**: $5 \text{ (Pipe)} + 2 \text{ (Overhead)} = \mathbf{7 \text{ Cycles}}$.
- **Behavior**: Instruction B can issue to the *second* SIMD unit at cycle 7, overlapping 25 cycles of execution with Instruction A.

---

## 4. AraO3 (Out-of-Order) Timing Model

### How it Works
In the `O3CPU`, chaining works by **event-driven decoupling**. We split the completion of an instruction into two separate simulator events.

### Key Code Changes
1.  **`src/cpu/o3/inst_queue.hh`**: Defined a new `WakeDependents` event.
2.  **`src/cpu/o3/inst_queue.cc`**: Modified `scheduleReadyInsts` to schedule both events.
    ```cpp
    // Early Event: Functional execution and dependent wakeup
    auto wakeup = new WakeDependents(issuing_inst, this);
    cpu->schedule(wakeup, cpu->clockEdge(Cycles(chaining_latency - 1)));

    // Late Event: Structural FU release and cleanup
    auto execution = new FUCompletion(issuing_inst, fu_pool, idx, this);
    cpu->schedule(execution, cpu->clockEdge(Cycles(op_latency - 1)));
    ```

### Stability Guards (Vital for O3)
- **Functional Order**: The `WakeDependents` event executes the instruction functionally *before* waking consumers to prevent reading invalid data (Fixes SEGVs).
- **Redundancy Guard**: Added `isResultReady()` check in `wakeDependents()` to prevent double-wakeups when `FUCompletion` eventually fires.
- **Memory Consistency**: Restricted early chaining to arithmetic instructions to maintain LSQ ordering.

---

## 5. Hardware Latency Alignment

The following table confirms the consistency between the ARA hardware pipeline depths and the gem5 `chainingLatency` implementation (which includes a fixed 2-cycle synchronization overhead).

| VFU / OpClass | Hardware Pipe Latency | gem5 `chainingLatency` | Status |
| :--- | :--- | :--- | :--- |
| **VFU_Alu** (Add/Logic) | 1 Cycle | 3 Cycles | **Match** |
| **VFU_Mul** (Integer) | 0 (EW8), 1 (others) | 2 or 3 Cycles | **Match** |
| **VFU_MFpu** (FP Add/Mul) | 2 (EW8) to 5 (EW64) | 4 to 7 Cycles | **Match** |
| **VFU_MFpu** (Conversion) | 2 Cycles | 4 Cycles | **Match** |
| **VFU_MFpu** (Div/Sqrt) | 3 Cycles | 5 Cycles | **Match** |
| **VFU_Div** (Integer Div) | 1-64 Cycles | 4 to 34 Cycles | **Consistent** |

### Key Consistency Notes:
- **Structural Hazards**: Non-pipelined units (Div/Sqrt) correctly block the functional unit for $Pipeline + Throughput$ cycles via the `dynamicOpLatency` override.
- **Data Hazards**: Dependent instructions are woken up early via `chainingLatency`, accurately modeling the element-streaming capability of the ARA hardware.

---

## 7. Lane Configuration & Parameter Mapping

To accurately match the AraO3 model to specific ARA hardware lane counts, use the following recommended parameters.

| ARA Hardware Lanes | `--vector-timing-throughput` | `--simd-units` | Modeling Intent | Min VLEN for Chaining* |
| :--- | :--- | :--- | :--- | :--- |
| **2 Lanes** | 2 | 2 | 2-lane streaming | > 896 bits |
| **4 Lanes** | 4 | 2 | 4-lane streaming | > 1792 bits |
| **8 Lanes** | 8 | 2 | 8-lane streaming | > 3584 bits |
| **16 Lanes** | 16 | 2 | 16-lane streaming | > 7168 bits |
| **32 Lanes** | 32 | 2 | 32-lane streaming | > 14336 bits |
| **64 Lanes** | 64 | 2 | 64-lane streaming | > 28672 bits |
| **128 Lanes** | 128 | 2 | 128-lane streaming | > 57344 bits |

*\*Calculated for FP64 (SEW=64) where Chaining Latency is ~7 cycles. Chaining only triggers if occupancy > readiness.*

### Key Configuration Logic

1.  **Why `--simd-units` stays at 2?**
    Even with many lanes, ARA is typically a **single-issue** engine. We use 2 units to model the "Bucket Brigade" overlap: Unit 1 handles the producer, and Unit 2 handles the consumer starting early. Setting this higher (e.g., 128) would incorrectly model a massive superscalar processor that could start 128 independent vector instructions in one cycle.

2.  **The "Vanishing Chaining" Effect**
    As you increase lanes, the hardware becomes so fast that it may finish an entire vector instruction before the first result even clears the pipeline. If your lanes finish in 4 cycles but the pipeline depth is 7, **chaining is impossible**. You must use very large `VLEN` values on wide-lane configurations to see the performance benefits of chaining.

---

## 8. Invocation & Usage

### Compilation
```bash
scons build/RISCV/gem5.opt -j$(nproc)
```

### Running AraMinor
Use `--simd-units 2` to provide hardware for the overlapped instruction to issue to.
```bash
./build/RISCV/gem5.opt rvv/riscv-rvv-se-ara.py <binary> \
    --cpu-type AraMinor \
    --enable-chaining \
    --simd-units 2 \
    --vector-timing-throughput 1 \
    --vlen 2048
```

### Running AraO3
```bash
./build/RISCV/gem5.opt rvv/riscv-rvv-se-ara.py <binary> \
    --cpu-type AraO3 \
    --enable-chaining \
    --simd-units 2 \
    --vector-timing-throughput 4 \
    --vlen 1024
```

#### Important Parameters for AraO3 Chaining
When running `AraO3`, two parameters are critical for achieving realistic chaining performance:
1.  **`--vector-timing-throughput`**: This represents the number of vector elements processed per cycle (modeling lane width). A value of **4** is common for high-performance configurations.
2.  **`--simd-units`**: This represents the number of independent functional units. **You must set this to at least 2** to see the benefits of chaining. This allows a dependent instruction to issue to the second unit while the producer still occupies the first.

*Note: These are now the default arguments in the `riscv-rvv-se-ara.py` script.*
