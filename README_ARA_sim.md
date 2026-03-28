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
| `src/cpu/o3/AraConfig.py` | Split FU pool; calibrated `opLat` values; `pipelined=False` on serial dividers |
| `src/cpu/o3/lsq_unit.hh` | Added `VectorLoadChainEvent` inner class |
| `src/cpu/o3/lsq_unit.cc` | Implemented load-chaining writeback delay event |
| `src/arch/riscv/insts/vector.hh` | `dynamicOpLatency()`, `chainingLatency()`, `DISPATCH_FLOOR`; AraXL cluster-aware throughput and reduction latency |
| `src/cpu/base.hh` / `base.cc` / `BaseCPU.py` | Added `vector_timing_model`, `nr_clusters`, `ring_latency` parameters |
| `rvv/riscv-rvv-se-ara.py` | Simulation script with `--enable-chaining`, `--vlen`, AraXL parameters |
| `rvv/Makefile.tests` | Build targets for all test binaries |

---

## Building

```bash
# From the gem5 root
scons build/RISCV/gem5.opt -j$(nproc)
```

---

## Running a Simulation

Use `rvv/riscv-rvv-se-ara.py` as the simulation script.

### ARA (single-cluster)

```bash
build/RISCV/gem5.opt rvv/riscv-rvv-se-ara.py \
    --enable-chaining \
    --vlen 512 \
    --vector-timing-throughput 4 \
    --simd-units 2 \
    /path/to/riscv-binary
```

### AraXL (multi-cluster)

```bash
# Explicit parameters
build/RISCV/gem5.opt rvv/riscv-rvv-se-ara.py \
    --enable-chaining \
    --vector-timing-model araxl \
    --vlen 1024 \
    --vector-timing-throughput 4 \
    --nr-clusters 2 \
    --simd-units 4 \
    /path/to/riscv-binary

# Shorthand flag (equivalent to above with default lane/cluster counts)
build/RISCV/gem5.opt rvv/riscv-rvv-se-ara.py \
    --enable-chaining --araxl --vlen 1024 \
    /path/to/riscv-binary
```

### All Parameters

| Parameter | Default | ARA | AraXL | Meaning |
|-----------|---------|-----|-------|---------|
| `--enable-chaining` | off | ✓ | ✓ | Enable vector chaining |
| `--disable-chaining` | — | ✓ | ✓ | Disable vector chaining |
| `--vlen` | 256 | ✓ | ✓ | Vector register length in bits |
| `--vector-timing-throughput` | 4 | ✓ | ✓ | **Number of lanes** (`NrLanes`) |
| `--simd-units` | 2 | ✓ | ✓ | Pipelined FU slots (see below) |
| `--vector-timing-model` | `ara` | ✓ | ✓ | `"ara"` or `"araxl"` |
| `--nr-clusters` | 1 | — | ✓ | Number of AraXL clusters |
| `--ring-latency` | 0 | — | ✓ | Inter-cluster ring pipeline stages |
| `--araxl` | — | — | ✓ | Shorthand: sets model=araxl, clusters=2, simd-units=4 |

---

## Parameter Guide

### `--vector-timing-throughput` = number of lanes (`NrLanes`)

Controls how many elements the simulated hardware processes per cycle:

```
elements_per_cycle = NrLanes × NrClusters × (ELEN / sew)
throughput_cycles  = ceil(vl / elements_per_cycle)
```

Set this to the lane count of the hardware configuration being modelled:

| Hardware | `--vector-timing-throughput` |
|---|---|
| 2 lanes | `2` |
| 4 lanes | `4` |
| 8 lanes | `8` |
| 16 lanes | `16` |

### `--simd-units` = pipelined FU slots

Controls the `count` on `AraSIMD_Pipelined`.

**For ARA: always use `--simd-units 2`.** The O3 chaining mechanism requires one FU slot for the producer and one for the consumer. Setting it to 1 prevents chaining; setting it above 2 incorrectly allows multiple independent vector ops in parallel.

**For AraXL: use `--simd-units 4`.** AraXL's larger MFPU result queue (4 vs 2 in ARA) means more in-flight FP results can compete for VRF write ports. A value of 4 models this increased concurrency.

| `--simd-units` | ARA | AraXL |
|---|---|---|
| `1` | Chaining broken | Chaining broken |
| `2` | **Correct** | Understates result queue |
| `4` | Overstates issue width | **Correct** |

### `--vector-timing-model` = `ara` or `araxl`

Selects the timing model variant:

- **`ara`** (default): single-cluster flat topology. `NrClusters` is ignored.
- **`araxl`**: multi-cluster ring topology. Enables two AraXL-specific behaviours:
  1. **EW8 FP latency fix**: `chainingLatency` for 8-bit FP ops returns 3 cycles (AraXL's `fpu_latency()` has no EW8 case, falling through to `LatFCompEW16=3`) rather than ARA's 2 cycles. Both hit `DISPATCH_FLOOR=6` at typical configurations but the distinction is preserved for future lower-floor work.
  2. **Cross-cluster reduction overhead**: for reduction instructions, `chainingLatency` adds `log2(NrClusters) × (1 + ring_latency)` extra cycles for the inter-cluster accumulation stages.

### `--nr-clusters` = number of AraXL clusters

Scales the throughput formula and the reduction latency:

```
elements_per_cycle = NrClusters × NrLanes × (ELEN / sew)
reduction_overhead = log2(NrClusters) × (1 + ring_latency)   [AraXL only]
```

| AraXL Hardware | `--nr-clusters` | `--vlen` (typical) |
|---|---|---|
| 2 clusters × 4 lanes | `2` | `1024` |
| 4 clusters × 4 lanes | `4` | `2048` |
| 2 clusters × 8 lanes | `2` | `2048` |

Set `--vlen = NrClusters × NrLanes × 1024` to match the full AraXL register file size.

### `--ring-latency` = inter-cluster ring pipeline stages

Number of pipeline registers on the AraXL inter-cluster ring (default 0). Only affects `chainingLatency` for reduction instructions when `--vector-timing-model araxl` is active.

Example — 2 clusters, 1 ring stage:
```
reduction chainingLatency = base + log2(2) × (1 + 1) = base + 2
```

### `--araxl` shorthand

Equivalent to `--vector-timing-model araxl --nr-clusters 2 --simd-units 4` without overriding any value already set explicitly on the command line.

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

### Pipeline latencies (`chainingLatency = max(pipe + 2, DISPATCH_FLOOR)`)

| Instruction class | ARA RTL pipeline depth | `chainingLatency` (VLEN=512, 4 lanes) |
|---|---|---|
| Integer ALU (`vadd`, `vsub`, …) | 1 | 6 (floor) |
| Integer Multiply (`vmul`, …) | 1 | 6 (floor) |
| Integer Divide EW32 (`vdiv`) | 16 | 18 |
| Integer Divide EW64 (`vdiv`) | 32 | 34 |
| FP Compute EW32 (`vfadd`, `vfmul`, `vfmacc`) | 4 | 6 (floor) |
| FP Compute EW64 | 5 | 7 |
| FP Non-Compute (`vfmin`, `vfmax`, …) | 1 | 6 (floor) |
| FP Divide / Sqrt EW32 | 3 | 6 (floor) |
| FP Convert EW32 | 2 | 6 (floor) |

`DISPATCH_FLOOR = 6` cycles — minimum chaining latency enforced by ARA's scoreboard.
`CHAINING_OVERHEAD = 2` cycles — VRF write + hazard-synchronisation delay added on top of the pipeline depth.

### `dynamicOpLatency` (FU occupancy)

```
dynamicOpLatency = max(pipeline_depth + throughput_cycles, DISPATCH_FLOOR)

throughput_cycles = ceil(vl / (NrClusters × NrLanes × ELEN/sew))
```

This is the number of cycles the FU is occupied, independent of chaining. For pipelined units a new instruction can enter the FU once `dynamicOpLatency` has elapsed for the previous one (or earlier if chaining is active).

### AraXL vs ARA latency differences

All pipeline-depth constants (`LatFComp*`, `LatFDivSqrt`, etc.) are **identical** between ARA and AraXL. The only differences visible in `chainingLatency` are:

| Scenario | ARA | AraXL |
|---|---|---|
| FP FMA/Add/Mul at EW8 | `pipeline_lat = 2` | `pipeline_lat = 3` (no EW8 case in RTL, falls to EW16) |
| Reduction instructions | no cross-cluster stages | `+log2(NrClusters) × (1+ring_latency)` cycles |
| Throughput (`elements_per_cycle`) | `NrLanes × (ELEN/sew)` | `NrClusters × NrLanes × (ELEN/sew)` |

At typical configurations (4 lanes, `DISPATCH_FLOOR=6`) the EW8 FP difference is invisible because both 2 and 3 cycles produce `chainingLatency = max(4,6) = 6`.

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

All test binaries are in `rvv/` and built with `Makefile.tests`:

```bash
cd rvv/
make -f Makefile.tests          # build all test binaries
```

### ARA latency checker (`check_ara_latencies.py`)

Runs `rvv_ara_latency_test.bin` under gem5 and validates all 12 per-instruction chaining latencies against expected ARA RTL values (VLEN=512, 4 lanes):

```bash
python3 rvv/check_ara_latencies.py \
    --gem5   build/RISCV/gem5.opt \
    --script rvv/riscv-rvv-se-ara.py \
    --binary rvv/rvv_ara_latency_test.bin \
    --vlen 512 --lanes 4
```

Expected result: 12/12 PASS. Exit code 0 on success.

### AraXL latency checker (`check_araxl_latencies.py`)

Two test suites in one script:

**Suite 1** — Standard AraXL config (NrLanes=4, NrClusters=2, VLEN=1024). Verifies all 12 instructions produce correct chaining latencies. All values are identical to ARA because `DISPATCH_FLOOR=6` dominates and the proportional increase in `NrClusters` and `VLEN` leaves `throughput_cycles` unchanged.

**Suite 2** — Throughput differentiation (NrLanes=1, VLEN=512). Compares `vfdiv_ew32` with and without clusters to verify that `NrClusters` correctly scales `elements_per_cycle` in `dynamicOpLatency`. The non-pipelined `count=1` FU makes this visible:

| Config | `vfdiv_ew32` | Formula |
|---|---|---|
| ARA (1L/1C) | ~88 cy | 8 micro-ops × max(3+8, 6) = 8×11 |
| AraXL (1L/2C) | ~56 cy | 8 micro-ops × max(3+4, 6) = 8×7 |

```bash
python3 rvv/check_araxl_latencies.py \
    --gem5     build/RISCV/gem5.opt \
    --script   rvv/riscv-rvv-se-ara.py \
    --binary   rvv/rvv_ara_latency_test.bin \
    --vlen 1024 --lanes 4 --clusters 2

# Skip the throughput suite (faster, latency-only check):
python3 rvv/check_araxl_latencies.py ... --skip-throughput
```

Expected result: Suite 1 12/12 PASS, Suite 2 PASS (AraXL faster than ARA). Exit code 0 on success.

### Load chain test (`run_load_chain_test.py`)

Tests that vector loads correctly chain with dependent compute instructions:

```bash
python3 rvv/run_load_chain_test.py \
    --gem5   build/RISCV/gem5.opt \
    --script rvv/riscv-rvv-se-ara.py \
    --binary rvv/rvv_load_chain_test.bin \
    --vlen 512 --lanes 4
```

### Running all tests

```bash
GEM5=build/RISCV/gem5.opt
SCRIPT=rvv/riscv-rvv-se-ara.py

# ARA regression
python3 rvv/check_ara_latencies.py \
    --gem5 $GEM5 --script $SCRIPT \
    --binary rvv/rvv_ara_latency_test.bin \
    --vlen 512 --lanes 4

# AraXL (both suites)
python3 rvv/check_araxl_latencies.py \
    --gem5 $GEM5 --script $SCRIPT \
    --binary rvv/rvv_ara_latency_test.bin \
    --vlen 1024 --lanes 4 --clusters 2

# Load chaining
python3 rvv/run_load_chain_test.py \
    --gem5 $GEM5 --script $SCRIPT \
    --binary rvv/rvv_load_chain_test.bin \
    --vlen 512 --lanes 4
```
