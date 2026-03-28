# ARA Vector Simulation in gem5

This guide describes the custom ARA timing model implemented in this gem5 fork, including vector chaining, the functional unit pool, and how to run and validate simulations.

For a step-by-step explanation of every code change — including worked examples and the reasoning behind each decision — see **[IMPLEMENTATION_TUTORIAL.md](IMPLEMENTATION_TUTORIAL.md)**.

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
| `src/arch/riscv/insts/vector.hh` | `dynamicOpLatency()`, `chainingLatency()`, `DISPATCH_FLOOR`; AraXL cluster-aware throughput and reduction latency; `SimdFloatCmpOp` (FP compare) pipeline depth fix |
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

| Instruction class | gem5 opClass | ARA RTL pipeline depth | `chainingLatency` (VLEN=512, 4 lanes) |
|---|---|---|---|
| Integer ALU (`vadd`, `vsub`, …) | `SimdAdd`, `SimdAlu`, … | 1 | 6 (floor) |
| Integer Multiply (`vmul`, …) | `SimdMult`, `SimdMultAcc` | EW8=0, else 1 | 6 (floor) |
| Integer Divide EW32 (`vdiv`) | `SimdDiv` | 16 | 18 |
| Integer Divide EW64 (`vdiv`) | `SimdDiv` | 32 | 34 |
| FP Compute EW32 (`vfadd`, `vfmul`, `vfmacc`) | `SimdFloatAdd`, `SimdFloatMult`, `SimdFloatMultAcc` | 4 | 6 (floor) |
| FP Compute EW64 | same | 5 | 7 |
| FP Non-Compute (`vfmin`, `vfmax`, `vfsgnj*`, `vfclass`) | `SimdFloatAlu` | 1 | 6 (floor) |
| FP Compare (`vmfeq`, `vmflt`, `vmfle`, `vmfne`, `vmfgt`, `vmfge`) | `SimdFloatCmp` | EW32=4, EW64=5 | 6 (floor) at EW32, **7** at EW64 |
| FP Divide / Sqrt EW32 | `SimdFloatDiv`, `SimdFloatSqrt` | 3 | 6 (floor) |
| FP Convert EW32 | `SimdFloatCvt` | 2 | 6 (floor) |
| FP Sum Reduction (`vfredusum`, `vfredosum`) | `SimdFloatReduceAdd` | EW32=4, EW64=5 | 6 / 7 |
| FP Min/Max Reduction (`vfredmin`, `vfredmax`) | `SimdFloatReduceCmp` | 1 | 6 (floor) |

`DISPATCH_FLOOR = 6` cycles — minimum chaining latency enforced by ARA's scoreboard.
`CHAINING_OVERHEAD = 2` cycles — VRF write + hazard-synchronisation delay added on top of the pipeline depth.

#### RTL source mapping

The pipeline depths above come from `ara_pkg.sv` `fpu_latency()`:

| RTL constant | Value | Instruction range |
|---|---|---|
| `LatFCompEW8` | 2 | (ARA only; EW8 FP compute) |
| `LatFCompEW16` | 3 | EW16 FP compute, AraXL EW8 (fallthrough) |
| `LatFCompEW32` | 4 | EW32 FP compute and **FP compare** (default case) |
| `LatFCompEW64` | 5 | EW64 FP compute and **FP compare** (default case) |
| `LatFNonComp` | 1 | `[VFMIN:VFSGNJX]`, `[VFREDMIN:VFREDMAX]` |
| `LatFConv` | 2 | `[VFCVTXUF:VFCVTFF]` |
| `LatFDivSqrt` | 3 | `VFDIV`, `VFRDIV`, `VFSQRT` |

**FP compare (vmfeq..vmfge) uses the default case** — these ops appear after all named ranges in the `ara_op_e` enum, so they fall to the `default:` branch of `fpu_latency()`, which returns `LatFComp*` (SEW-dependent), not `LatFNonComp=1`. The gem5 model was updated to use `pipeline_lat = vsew + 2` for `SimdFloatCmpOp` to match.

### `dynamicOpLatency` (FU occupancy)

```
dynamicOpLatency = max(pipeline_depth + throughput_cycles, DISPATCH_FLOOR)

throughput_cycles = ceil(vl / (NrClusters × NrLanes × ELEN/sew))
```

This is the number of cycles the FU is occupied, independent of chaining. For pipelined units a new instruction can enter the FU once `dynamicOpLatency` has elapsed for the previous one (or earlier if chaining is active).

### DISPATCH_FLOOR and WakeDependents interaction

Vector chaining is implemented via the `WakeDependents` mechanism in `inst_queue.cc`. The key rule:

> **`WakeDependents` fires only when `chainingLatency < dynamicOpLatency`.**

For all pipelined FP ops at typical configurations (VLEN=512, 4 lanes, LMUL=m8):

```
throughput_cycles = ceil(microVl / epc) = ceil(16 / 8) = 2
dynamicOpLatency = max(0 + 2, 6) = 6   [DISPATCH_FLOOR dominates]
```

When `chainingLatency ≥ dynamicOpLatency`, the consumer wakes via `FUCompletion` at `dynamicOpLatency`, not via `WakeDependents` at `chainingLatency`. This means:

- For `vfadd_ew32` (CL=6): CL == dynamicOpLatency=6 → `WakeDependents` does NOT fire; consumer wakes at 6.
- For `vfadd_ew64` (CL=7): CL > dynamicOpLatency=6 → same conclusion; consumer wakes at 6.
- For `vmfeq_ew64` (CL=7, fixed from 6): same as above — the fix is correct per RTL but **not observable** at VLEN=512, 4 lanes with DISPATCH_FLOOR=6.

The fix to `SimdFloatCmpOp` would become observable if:
- `DISPATCH_FLOOR` were lowered, or
- `dynamicOpLatency` exceeded 7 (requires `throughput_cycles > 7`, i.e. `microVl > 7 × NrLanes`)

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

Runs `rvv_ara_latency_test.bin` under gem5 and validates 15 per-instruction chaining latencies against expected ARA RTL values (VLEN=512, 4 lanes):

```bash
python3 rvv/check_ara_latencies.py \
    --gem5   build/RISCV/gem5.opt \
    --script rvv/riscv-rvv-se-ara.py \
    --binary rvv/rvv_ara_latency_test.bin \
    --vlen 512 --lanes 4
```

Expected result: 15/15 PASS. Exit code 0 on success.

#### Tested instructions and expected gem5 measurements

| Test | Expected (gem5) | ARA RTL CL | Note |
|---|---|---|---|
| `vadd_ew32` | 8 | 6 | ALU CL=6 + O3 overhead |
| `vmul_ew32` | 8 | 6 | Mul CL=6 + O3 overhead |
| `vdiv_ew32` | 24 | 18 | vdiv(18) + vadd(CL=6) per iter |
| `vdiv_ew64` | 40 | 34 | vdiv(34) + vadd(CL=6) per iter |
| `vfadd_ew32` | 8 | 6 | FPComp EW32 CL=6 + O3 overhead |
| `vfadd_ew64` | 8 | 7 | FPComp EW64 CL=7; dynamicOpLatency=6 dominates |
| `vfmul_ew32` | 8 | 6 | Same FU as vfadd_ew32 |
| `vfmacc_ew32` | 8 | 6 | Same FU as vfadd_ew32 |
| `vfmin_ew32` | 8 | 6 | FPNonComp CL=6 + O3 overhead |
| `vfmin_ew64` | 8 | 6 | FPNonComp CL=6 (LatFNonComp=1 regardless of SEW) |
| `vmfeq_ew64` | 18 | 7 | FPCmp EW64; CL=7 but mask-register serialisation dominates (see below) |
| `vfredusum_ew64` | 12 | 7 | FP reduce-sum; full reduction occupancy |
| `vfdiv_ew32` | 48 | 6 | 8 micro-ops × dynamicOpLatency(6), non-pipelined |
| `vfsqrt_ew32` | 48 | 6 | 8 micro-ops × dynamicOpLatency(6), non-pipelined |
| `vfcvt_ew32` | 8 | 6 | FPConv CL=6 + O3 overhead (per-op avg) |

#### vmfeq_ew64 — why the measured value is 18, not 13

The test uses a `vmfeq → vfmerge` chain at LMUL=m1. The theoretical chain spacing from chaining latencies alone is CL(vmfeq_EW64=7) + CL(vfmerge_EW64=6) = 13 cycles. However the measured value is ~18 cycles because:

1. **WakeDependents does not fire.** For all pipelined FP ops, `dynamicOpLatency = max(0 + throughput_cycles, DISPATCH_FLOOR) = 6`. Since `CL=7 > dynamicOpLatency=6`, the consumer wakes via `FUCompletion` at `dynamicOpLatency=6`, not via `WakeDependents` at `CL=7`. The 1-cycle CL improvement from the `SimdFloatCmpOp` fix is therefore not directly observable.

2. **Mask-register serialisation.** `vmfeq` writes a mask register. gem5's register pinning mechanism (`getNumPinnedWritesToComplete`) requires all micro-ops of `vmfeq` to complete before the consumer `vfmerge` can read the mask. With LMUL=m1 this is 2 micro-ops; the serialisation adds ~5 cycles on top of the nominal `dynamicOpLatency`.

The `vmfeq_ew64` test still validates the `SimdFloatCmpOp` code path and confirms no regression in mask-generating instruction behaviour.

### AraXL latency checker (`check_araxl_latencies.py`)

Two test suites in one script:

**Suite 1** — Standard AraXL config (NrLanes=4, NrClusters=2, VLEN=1024). Verifies all 15 instructions produce correct chaining latencies. All values are identical to ARA because `DISPATCH_FLOOR=6` dominates and the proportional increase in `NrClusters` and `VLEN` leaves `throughput_cycles` unchanged.

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

Expected result: Suite 1 15/15 PASS, Suite 2 PASS (AraXL faster than ARA). Exit code 0 on success.

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

# ARA regression (15 tests)
python3 rvv/check_ara_latencies.py \
    --gem5 $GEM5 --script $SCRIPT \
    --binary rvv/rvv_ara_latency_test.bin \
    --vlen 512 --lanes 4

# AraXL (Suite 1: 15 tests + Suite 2: throughput differentiation)
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

---

## RTL Fidelity Verification

The table below documents how each gem5 opClass maps to the RTL `fpu_latency()` function in `ara_pkg.sv`, and confirms that the implementation is faithful to the RTL for all instruction classes.

| gem5 opClass | Instructions | RTL `fpu_latency()` branch | `pipeline_lat` (ARA) | `pipeline_lat` (AraXL) | Match? |
|---|---|---|---|---|---|
| `SimdFloatAddOp` | `vfadd`, `vfsub` | default → LatFComp* | `vsew+2` | `vsew+2` (EW8→3) | ✓ |
| `SimdFloatMultOp` | `vfmul` | default → LatFComp* | `vsew+2` | `vsew+2` (EW8→3) | ✓ |
| `SimdFloatMultAccOp` | `vfmacc`, `vfmsac`, … | default → LatFComp* | `vsew+2` | `vsew+2` (EW8→3) | ✓ |
| `SimdFloatAluOp` | `vfmin`, `vfmax`, `vfsgnj*`, `vfclass` | `[VFMIN:VFSGNJX]` → LatFNonComp=1 | 1 | 1 | ✓ |
| `SimdFloatCmpOp` | `vmfeq`, `vmfne`, `vmflt`, `vmfle`, `vmfgt`, `vmfge` | default → LatFComp* (falls through all named ranges) | `vsew+2` | `vsew+2` (EW8→3) | ✓ (fixed) |
| `SimdFloatCvtOp` | `vfcvt*`, `vfwcvt*`, `vfncvt*` | `[VFCVTXUF:VFCVTFF]` → LatFConv=2 | 2 | 2 | ✓ |
| `SimdFloatDivOp` | `vfdiv`, `vfrdiv` | LatFDivSqrt=3 | 3 | 3 | ✓ |
| `SimdFloatSqrtOp` | `vfsqrt` | LatFDivSqrt=3 | 3 | 3 | ✓ |
| `SimdDivOp` | `vdiv`, `vremu`, … | (integer, not fpu_latency) 4<<vsew | `4<<vsew` | `4<<vsew` | ✓ |
| `SimdMultOp` / `SimdMultAccOp` | `vmul`, `vmacc`, … | (integer) EW8=0, else 1 | EW8=0, else 1 | same | ✓ |
| `SimdFloatReduceAddOp` | `vfredusum`, `vfredosum` | default → LatFComp* | `vsew+2` | `vsew+2` + cross-cluster | ✓ |
| `SimdFloatReduceCmpOp` | `vfredmin`, `vfredmax` | `[VFREDMIN:VFREDMAX]` → LatFNonComp=1 | 1 | 1 + cross-cluster | ✓ |
| AraXL throughput scaling | all ops | `epc = NrClusters×NrLanes×(ELEN/sew)` | N/A | ✓ | ✓ |
| AraXL cross-cluster reduction | reduce ops | `log2(NrClusters)×(1+ringLat)` | N/A | ✓ | ✓ |

### `SimdFloatCmpOp` fix detail

Prior to the fix, `SimdFloatCmpOp` fell to the `default: pipeline_lat = 1` catch-all in `chainingLatency()`, giving CL=6 at all SEW values. The correct value from RTL is `LatFComp*` (the default case in `fpu_latency()`), giving:

- EW32: `pipeline_lat = 4` → CL = max(4+2, 6) = 6 (unchanged — DISPATCH_FLOOR dominates)
- EW64: `pipeline_lat = 5` → CL = max(5+2, 6) = **7** (was 6 — off by one)

The fix is semantically correct. Its timing effect is not observable at VLEN=512, 4 lanes because `dynamicOpLatency` is clamped to `DISPATCH_FLOOR=6 < CL=7`, so `WakeDependents` never fires regardless of the CL value. The fix would become observable if `dynamicOpLatency > 7` (e.g. fewer lanes, higher LMUL, or lower DISPATCH_FLOOR).
