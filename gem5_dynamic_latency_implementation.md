# Dynamic RISC-V Vector Latency Scaling in gem5

This document details how we modified gem5 to support dynamically scaled execution latencies for RISC-V vector instructions based on `vtype` CSR's `SEW` (Standard Element Width) and `LMUL` (Length Multiplier) fields, mimicking the hardware boundaries of the ARA core.

## 1. Problem Statement
By default, gem5's CPU timing models (both `O3CPU` and `MinorCPU`) assign static `opLat` lengths defined in the configuration files (like `AraConfig.py` mapping to `FUPool` configurations). This fails to emulate pipelined processing units which should depend heavily on the sizes of the instructions (LMUL and SEW).

## 2. Implementing `dynamicOpLatency`
We bypassed the static latency fetching mechanism in gem5 by utilizing the `StaticInst` abstraction layer.

### Modifying `StaticInst`
*File*: `src/cpu/static_inst.hh`
We added a new virtual runtime-latency dispatch virtual method:
```cpp
/// Dynamic Latency. Used by CPU to override FUPool when instructed.
virtual Cycles dynamicOpLatency(ThreadContext *tc) const { return Cycles(0); }
```

### Extending `VectorMicroInst` for SEW Variations
*File*: `src/arch/riscv/insts/vector.hh`
For vectors, we parse the `ThreadContext` extracting the `PCState` `vtype`. 
```cpp
Cycles dynamicOpLatency(ThreadContext *tc) const override
{
    auto &pcstate = tc->pcState().as<RiscvISA::PCState>();
    uint32_t current_vsew = pcstate.vtype().vsew;
    if (opClass() == SimdDivOp) {
        uint32_t op_lat = 4 << current_vsew; // SEW=8 -> 4 cycles. SEW=64 -> 32 cycles.
        return Cycles(op_lat);
    } else if (opClass() == SimdFloatAddOp || opClass() == SimdFloatCvtOp || 
               opClass() == SimdFloatMultOp || opClass() == SimdFloatMultAccOp || 
               opClass() == SimdFloatReduceOp) {
        uint32_t op_lat = current_vsew + 2; // SEW=8 -> 2 cycles. SEW=64 -> 5 cycles.
        return Cycles(op_lat);
    }
    return Cycles(0);
}
```

*Why does a larger Element Width (SEW) increase Latency?*
In hardware pipelines, processing wider data elements (like a 64-bit float vs a 16-bit float) requires more sequential logic stages. For example, in the ARA architecture, the division unit processes inputs sequentially through iterative stages. A 64-bit division (`SEW=64`) mathematically requires more cycles to resolve all 64 bits compared to resolving a 16-bit division (`SEW=16`). Therefore, because dividing wider elements simply takes more physical clock cycles to finish passing through the execution hardware, the pipeline latency increases proportionally with the SEW configuration.*Why does Element Width (SEW) change Latency?*
In the ARA hardware architecture, functional units process a fixed width of data per cycle (e.g., 64 bits per lane). When computing on narrower elements (like `SEW=16`), more vector elements can be packed into each parallel cycle group (4 elements per 64-bit chunk) than larger ones (like `SEW=64`, returning only 1 element per 64-bit chunk). Thus, mathematically operations like Division (`SimdDivOp`) take longer strictly based on the size of the elements passing through the sequential stages of the ALU.

## 3. Modifying CPU Schedulers to Support the Hook
We replaced the strict parameter lookup fetching with an evaluation hook that defers to `dynamicOpLatency` where available.

### Out of Order (O3) CPU Model
*File*: `src/cpu/o3/inst_queue.cc` (in `InstructionQueue::scheduleReadyInsts`)
```cpp
if (auto dyn_lat = issuing_inst->staticInst->dynamicOpLatency(issuing_inst->tcBase()); dyn_lat > Cycles(0)) {
    op_latency = dyn_lat;
} else {
    op_latency = fu_pool->getOpLatency(op_class);
}
```

### Minor CPU Model
*File*: `src/cpu/minor/execute.cc` (in `MinorCPU::Execute::issue`)
```cpp
Cycles inst_opLat = fu->description.opLat;
if (auto dyn_lat = inst->staticInst->dynamicOpLatency(cpu.getContext(thread_id)); dyn_lat > Cycles(0)) {
    inst_opLat = dyn_lat;
}
```

## 4. Handling LMUL (Length Multiplier) natively
Gem5's vector ISA implementation naturally handles `LMUL` through **Micro-Op Splitting**.
In `src/arch/riscv/isa/templates/vector_arith.isa`, macro instructions distribute loop limits across `num_microops = vtype_regs_per_group(vtype)`, creating one micro-operation per architectural pipeline block inside the instruction.

Thus, the instruction issue pipeline queues structurally queue items, automatically limiting overlapping functional unit dependencies (hazards), mirroring how `MAX(1, LMUL)` dictates the actual issue-time cost (occupancy) of hardware functional pipelines!
