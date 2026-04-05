# ARA Vector Simulation in gem5

This guide describes the custom ARA timing model implemented in this gem5 fork, including vector chaining, the functional unit pool, and how to run and validate simulations.

---

## Overview

The model replaces gem5's default 1-cycle vector latencies with values derived from the ARA RTL (`ara_pkg.sv`). It adds three capabilities not present in upstream gem5:

1. **Dynamic operation latency** — throughput scales with `VLEN`, element width, and lane count.
2. **Vector chaining** — a consumer instruction can start executing as soon as the producer's first result element is available, without waiting for the full vector to complete.
3. **Cache-miss-accurate load chaining** — vector loads participate in chaining based on actual memory latency, not a fixed timer.

---

## Files Changed

| File | What changed |
|------|-------------|
| `src/cpu/LatencyModel.py` | New: Modular latency SimObject interface |
| `src/cpu/latency_model.hh/.cc` | New: Implementation of the latency model strategy |
| `src/cpu/o3/AraConfig.py` | Implements `AraLatencyModel`; calibrated reconciled latencies |
| `src/cpu/o3/lsq_unit.hh/.cc` | Implemented load-chaining writeback delay event |
| `src/arch/riscv/insts/vector.hh` | `dynamicOpLatency()` refactored to query modular model |
| `rvv/riscv-rvv-se-ara.py` | Simulation script with `--cpu-type AraO3` support |

---

## Building

```bash
# From the gem5 root
scons build/RISCV/gem5.opt -j$(nproc)
```

---

## Running a Simulation

Use `rvv/riscv-rvv-se-ara.py` as the simulation script:

```bash
build/RISCV/gem5.opt rvv/riscv-rvv-se-ara.py \
    --enable-chaining \
    --vlen 512 \
    --vector-timing-throughput 4 \
    --simd-units 2 \
    /path/to/riscv-binary
```

### Key Parameters

| Parameter | Default | Meaning |
|-----------|---------|---------|
| `--enable-chaining` | off | Enable vector chaining (WakeDependents + load chain event) |
| `--vlen` | 512 | Vector register length in bits |
| `--vector-timing-throughput` | 4 | **Number of ARA lanes** — sets `NrLanes` in the throughput formula |
| `--simd-units` | 2 | FU pool slots for pipelined units (see below) |

---

## Parameter Guide: `--vector-timing-throughput` and `--simd-units`

These two parameters serve distinct purposes and should not be confused.

### `--vector-timing-throughput` = number of ARA lanes

This directly controls how many elements the simulated hardware processes per cycle. The throughput formula in `vector.hh` is:

```
elements_per_cycle = NrLanes × (ELEN / sew)
throughput_cycles  = ceil(vl / elements_per_cycle)
```

Set this to match the lane count of the ARA configuration you are modelling:

| ARA Hardware | `--vector-timing-throughput` |
|---|---|
| 2 lanes | `2` |
| 4 lanes | `4` |
| 8 lanes | `8` |
| 16 lanes | `16` |

### `--simd-units` = FU slots for the pipelined unit (keep at 2)

This controls the `count` on `AraSIMD_Pipelined` — the number of independent FU instances gem5 can dispatch pipelined vector instructions to simultaneously.

**Always use `--simd-units 2` regardless of lane count.** Here is why:

gem5's O3 chaining mechanism works by firing `WakeDependents` early (at `chainingLatency` cycles) and allowing the consumer instruction to *issue* while the producer still occupies one FU slot. This requires at least two FU slots — one for the producer, one for the consumer. Setting `simd_units=1` prevents chaining from activating even when the hardware would allow it.

Setting it higher than 2 (e.g. matching the lane count) would allow gem5 to dispatch many independent vector instructions simultaneously, incorrectly modelling ARA as a wide-issue superscalar vector engine.

| `--simd-units` | Effect |
|---|---|
| `1` | Chaining disabled (consumer can't issue while producer runs) |
| `2` | Correct: one slot for producer, one for consumer — matches ARA's single-issue-per-VFU model with chaining |
| `> 2` | Incorrect: multiple independent vector ops in parallel, overstates ARA's issue width |

---

## Functional Unit Pool

The FU pool is split into three classes to correctly model ARA's structural hazards:

### `AraSIMD_Pipelined` (count = `simd_units`, default 2)

Covers all pipelined VFUs: integer ALU/Mul, FP compute/compare/convert/reduce, and load/store address generation. Two slots are provided to support the WakeDependents chaining mechanism.

### `AraSIMD_IntDiv` (count = 1)

Integer divide only (`SimdDiv`, `pipelined=False`). ARA has one serial integer divider; `count=1` enforces that two independent `vdiv` instructions cannot execute simultaneously.

### `AraSIMD_FPDivSqrt` (count = 1)

FP divide and square root (`SimdFloatDiv`, `SimdFloatSqrt`, `pipelined=False`). fpnew's DIVSQRT unit is iterative — one operation at a time. `count=1` enforces this constraint.

> **Why it matters:** without the split, `count=2` on a single FU class would allow two simultaneous `vdiv` or `vfdiv` instructions, halving the apparent latency for kernels with independent divide-heavy loops.

---

## Latency Model

The timing model is **modular**. The core ISA code in `vector.hh` is generic and queries a `LatencyModel` SimObject assigned to the CPU. The ARA-specific implementation is defined in Python within `AraConfig.py`.

### Reconciled Pipeline Latencies (RTL-Accurate)

The following values represent the **Instruction-to-Instruction (Iss-to-Iss)** dependency delay. They are implemented in `AraLatencyModel` and reconcile gem5 with measurements from the ARA RTL Verilator simulation.

| Instruction Category | RTL Meas. (Iss-to-Iss) | gem5 `op_latency` | Total (1+Lat) | Logic |
| :--- | :---: | :---: | :---: | :--- |
| **Integer ALU** (`vadd`) | 7 | 6 | 7 | Sequencer floor |
| **FP Add/Mul (EW32)** | 11 | 10 | 11 | `vsew + 8` |
| **FP Add/Mul (EW64)** | 12 | 11 | 12 | `vsew + 8` |
| **FP Div/Sqrt (EW32)**| 20 | 19 | 20 | Iterative SRT |
| **Integer Div (EW32)** | 42 | 41 | 42 | `(8 << vsew) + 9`|
| **Memory Load (Hit)** | 24 | 23 | 24 | AGU + Sync |

### Time Buffer Depth

To support long-latency operations (like the 74-cycle 64-bit division) without triggering gem5 assertions, the `AraO3CPU` increases the depth of its internal communication buffers:
* **`backComSize = 100`**
* **`forwardComSize = 100`**

### Modular extensibility

To implement a different architecture, define a new subclass of `LatencyModel` in Python and assign it to the CPU's `latency_model` parameter. No C++ changes are required to add new instruction-class timing tables.

---

## Vector Chaining

### Compute chaining (`WakeDependents`)

When chaining is enabled and `chainingLatency < dynamicOpLatency` (i.e. the vector is long enough that the FU will still be busy when the first result element is ready), gem5 schedules a `WakeDependents` event at `chainingLatency` cycles after issue. The consumer instruction is placed back in the issue queue and can start as soon as a free FU slot is available.

The minimum vector length required to activate chaining for FP EW64 (the tightest case, `chainingLatency = 7`):

```
throughput_cycles > 7  →  vl > 7 × NrLanes
```

| Lanes | Min `vl` (EW64, LMUL=m1) | Min `VLEN` |
|---|---|---|
| 2 | > 14 elements | > 896 bits |
| 4 | > 28 elements | > 1792 bits |
| 8 | > 56 elements | > 3584 bits |

For LMUL=m8 the effective `vl` is 8× larger, so VLEN=512 activates chaining at 4 lanes for all instruction classes.

### Load chaining (`VectorLoadChainEvent`)

Vector loads cannot use `WakeDependents` because the data arrives from the cache, not from a fixed-latency FU. Instead, `LSQUnit::writeback()` is hooked: after `completeAcc()` makes the loaded data available, a 1-cycle `VectorLoadChainEvent` fires before the instruction is committed. This models the 1-cycle VRF-write overhead seen in ARA hardware, and works correctly for both L1 hits and cache misses.

The total load-chain overhead is 2 cycles (1 explicit delay + 1 IEW pipeline stage).

---

## Test Suite

All tests are in `rvv/` and built with `Makefile.tests`:

```bash
cd rvv/
make -f Makefile.tests          # build all test binaries
```

### Latency tests (`rvv_ara_latency_test.c`)

Measures per-instruction chaining latency for each ARA VFU category using RAW dependency chains. Output format: `LATENCY <name>: avg=<X.XX>`.

This binary contains no gem5-specific APIs — it uses only `rdcycle` (standard RISC-V) and `printf`. It can be run directly on ARA RTL hardware.

### Latency checker (`check_ara_latencies.py`)

Runs `rvv_ara_latency_test.bin` under gem5 and validates measured latencies against expected values:

```bash
python3 rvv/check_ara_latencies.py \
    --gem5   build/RISCV/gem5.opt \
    --script rvv/riscv-rvv-se-ara.py \
    --binary rvv/rvv_ara_latency_test.bin \
    --vlen 512 --lanes 4
```

### Load chain test (`rvv_load_chain_test.c` + `run_load_chain_test.py`)

Tests that vector loads correctly chain with dependent compute instructions, with and without chaining enabled:

```bash
python3 rvv/run_load_chain_test.py \
    --gem5   build/RISCV/gem5.opt \
    --script rvv/riscv-rvv-se-ara.py \
    --binary rvv/rvv_load_chain_test.bin \
    --vlen 512 --lanes 4
```
