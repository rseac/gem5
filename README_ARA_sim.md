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
| `src/arch/riscv/insts/vector.hh` | **Fixed**: `dynamicOpLatency()` now calculates occupancy dynamically using `microVl` and CPU throughput. |
| `rvv/riscv-rvv-se-ara.py` | Simulation script with `--cpu-type AraO3` support |
| `rvv/rvv_test.cpp` | **Updated**: Switched to `uint8_t` to maximize vector occupancy for lane verification. |

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
occupancy_cycles = ceil(microVl / vectorTimingThroughput)
total_latency    = pipeline_depth + occupancy_cycles
```

Set this to match the lane count of the ARA configuration you are modelling:

| ARA Hardware | `--vector-timing-throughput` |
|---|---|
| 2 lanes | `2` |
| 4 lanes | `4` |
| 8 lanes | `8` |
| 16 lanes | `16` |

**Note**: As of the latest update, gem5 is sensitive to this parameter. Changing throughput will scale the execution cycles of vector instructions.

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

The timing model is **modular and dynamic**. The core ISA code in `vector.hh` calculates instruction occupancy based on the vector length and lane count, then adds it to the pipeline depth queried from a `LatencyModel` SimObject.

### Reconciled Pipeline Latencies (RTL-Accurate)

The following values represent the **fixed pipeline depth** (Instruction-to-Instruction dependency delay for a single element). They are implemented in `AraLatencyModel` within `AraConfig.py`.

| Instruction Category | RTL Meas. (Iss-to-Iss) | gem5 `pipe_depth` | Total Latency (Pipe + Occ) | Logic |
| :--- | :---: | :---: | :---: | :--- |
| **Integer ALU** (`vadd`) | 7 | 6 | $6 + \lceil vl/thru \rceil$ | Sequencer floor |
| **FP Add/Mul (EW32)** | 11 | 10 | $10 + \lceil vl/thru \rceil$ | `vsew + 8` |
| **FP Add/Mul (EW64)** | 12 | 11 | $11 + \lceil vl/thru \rceil$ | `vsew + 8` |
| **FP Div/Sqrt (EW32)**| 20 | 19 | $19 + \lceil vl/thru \rceil$ | Iterative SRT |
| **Integer Div (EW32)** | 42 | 41 | $41 + \lceil vl/thru \rceil$ | `(8 << vsew) + 9`|
| **Memory Load (Hit)** | 24 | 23 | $23 + \lceil vl/thru \rceil$ | AGU + Sync |

### Dynamic Throughput Scaling

Total execution latency is now calculated as:
$$\text{Latency} = \text{Pipeline Depth} + \lceil \text{microVl} / \text{vectorTimingThroughput} \rceil$$

This ensures that:
1. Large vectors correctly occupy the functional units for more cycles.
2. Increasing the `--vector-timing-throughput` (adding lanes) correctly reduces the execution time of long instructions.
3. The "Vanishing Chaining" effect is accurately modeled: if $\lceil vl/thru \rceil \le \text{chainingLatency}$, chaining is automatically disabled as the instruction finishes before the first result is ready.

---

## Vector Chaining

### Compute chaining (`WakeDependents`)

When chaining is enabled and `chainingLatency < dynamicOpLatency` (i.e. the vector is long enough that the FU will still be busy when the first result element is ready), gem5 schedules a `WakeDependents` event at `chainingLatency` cycles after issue. The consumer instruction is placed back in the issue queue and can start as soon as a free FU slot is available.

### Load chaining (`VectorLoadChainEvent`)

Vector loads cannot use `WakeDependents` because the data arrives from the cache, not from a fixed-latency FU. Instead, `LSQUnit::writeback()` is hooked: after `completeAcc()` makes the loaded data available, a 1-cycle `VectorLoadChainEvent` fires before the instruction is committed. 

---

## Test Suite

### Dynamic Throughput Verification (`rvv/rvv_test.cpp`)

A benchmark designed to verify lane scaling. It uses `uint8_t` (SEW=8) to maximize element count per vector.
*   **VLEN=4096, Throughput=4**: Occupancy = 128 cycles.
*   **VLEN=4096, Throughput=6**: Occupancy = 86 cycles.
*   **VLEN=256, Throughput=6**: Demonstrates "Vanishing Chaining" (6 cycles occupancy < 7 cycles chaining latency).
