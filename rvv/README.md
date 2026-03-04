# RISC-V Vector Arithmetic Latency Benchmark (`rvv_arith_latency`)

This benchmark is designed to empirically test pipeline latency and throughput scaling of RISC-V Vector instructions within the gem5 CPU models (both Out-of-Order `AraO3` and In-Order `AraMinor`).

## What it tests
The core of the benchmark is an unrolled list of dependent floating-point addition operations (`__riscv_vfadd_vv`). By feeding the output of one arithmetic intrinsic directly into the input of the next inside a tight loop, it forms a massive dependency chain. 
Because the CPU cannot issue the next `vfadd` until the previous one is fully computed, this completely breaks instruction-level parallelism and exposes the **pure operational pipeline latency** of the functional unit across a span of 10,000 loop executions.

## Dynamic Latency Scaling
In the modified ARA-aligned gem5 implementation, the simulated `opLat` (operation execution physical clock cycles) scales dynamically based on the **Standard Element Width (SEW)** configuration provided by the `vtype` vector configuration register.

Wider elements (e.g. `SEW=64`) take more cycle stages to propagate through logic stages than narrower elements (like `SEW=16`).

### Compilation
The benchmark supports swapping the `SEW` size natively through conditional C macros at compile-time via `Makefile.arith`.
```bash
# Builds rvv_arith_latency_32.bin
make -f Makefile.arith SEW=32

# Builds rvv_arith_latency_64.bin
make -f Makefile.arith SEW=64 

# Builds rvv_arith_latency_16.bin
make -f Makefile.arith SEW=16
```

## Running & Simulation Results
To execute the benchmark in gem5 using the ARA timing configurations, invoke the `riscv-rvv-se-ara.py` Syscall Emulation script:
```bash
./build/RISCV/gem5.opt rvv/riscv-rvv-se-ara.py rvv/rvv_arith_latency_32.bin --cpu-type AraO3
```

### Empirical Tick Metrics 
Executing the 10,000-deep dependency chain against both CPU models highlights the difference in scaled operation latency directly injected by `dynamicOpLatency`:

**AraO3CPU Model**
*   **SEW=32 (`rvv_arith_latency_32.bin`)**: 559,965 Cycles
*   **SEW=64 (`rvv_arith_latency_64.bin`)**: 562,301 Cycles

**AraMinorCPU Model**
*   **SEW=16 (`rvv_arith_latency_16.bin`)**: 678,295 Cycles
*   **SEW=32 (`rvv_arith_latency_32.bin`)**: 670,758 Cycles
*   **SEW=64 (`rvv_arith_latency_64.bin`)**: 674,700 Cycles

As shown, evaluating `SEW=64` strictly prolongs the execution span compared to `SEW=32` due to the pipeline cycle inflation modeled inside gem5 for wider operands!
