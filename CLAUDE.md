# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## What this fork is

This is a fork of gem5 that adds an **ARA RISC-V Vector timing model** to the O3 CPU. The three things it adds on top of upstream gem5 are:

1. **Dynamic vector op latency** — FU occupancy scales with `VLEN`, `SEW`, and lane count instead of a fixed 1-cycle latency.
2. **Compute chaining** — consumer issues as soon as the producer's first element is ready, via `WakeDependents` at `chainingLatency` cycles.
3. **Cache-miss-accurate load chaining** — vector loads chain through a 1-cycle `VectorLoadChainEvent` fired after `completeAcc()`, so chaining respects real memory latency rather than a fixed timer.

`README_ARA_sim.md` is the authoritative reference for the model, parameters, and reconciled latency values. Read it before changing latency math or FU pool configuration.

## Goals

I want to use this gem5 Ara simulator to model hardware prefetchers for the RISC-V vector extension. The goal is to either improve upon existing designs for vector workloads or create a new design entirely. With that in mind, I'll need to modify lots of the components and scripts provided in this repo to fine-tune experiements to custom prefetchers. In general, I want code to be parameterized as possible, so configuring hardware can easily be done with command line arguments.

## Notes

rvv/rvv/riscv-rvv-se-ara.py is the original syscall emulation script provided by this repo,
while the script I'm using for experiments is rvv/riscv-rvv-se-ara-prefetcher.py.

Test kernels are located at tests/test-progs/custom/src.

## Documentation

Every new feature you add should be documented in DOCUMENTATION.MD. For each new change, you should explain the existing method gem5 used and what you added on top of it. Make sure to cite files in the codebase so its clear where you are drawing your conclusions from, as gem5's documentation is very limited.

## Best Practices

When extending gem5 features, do NOT modify existing core files. For example, if I wanted to experiement with a custom cache hierarchy, I would not want you to implement your changes in one of the core cache hierarchy files that already exists.

In general, I want you to extend gem5 features by utilizing the existing infrastructure in this codebase. It is unlikely you'll ever need to create classes/objects from scratch, and it is often better to utilize existing classes/objects instead.

Finally, the code you write should be as simple as possible. It should be reasonably straightforward to understand for anyone with a basic understanding of gem5, and you should not rely on any exotic C++/Python features to implement changes (such as monkeypatches).

## Build

Whenever any files are edited in the src directory, I will build it myself in my docker shell. Do not run any build commands yourself, simply notify me when I need to run a build.

## Running an ARA simulation

```bash
build/RISCV/gem5.opt rvv/riscv-rvv-se-ara-prefetcher.py \
    --enable-chaining \
    --vlen 512 \
    --vector-timing-throughput 4 \
    --simd-units 2 \
    /path/to/riscv-binary
```

- `--vector-timing-throughput` = **ARA lane count** (2/4/8/16). Drives the throughput formula in `src/arch/riscv/insts/vector.hh`.
- `--simd-units` should stay at **2** regardless of lane count. It is the FU pool slot count for `AraSIMD_Pipelined`; chaining needs one slot for the producer and one for the consumer. The integer divider and FP div/sqrt pools are forced to `count=1, pipelined=False` to model ARA's serial dividers.

## Tests

ARA test binaries live in `rvv/` and are built with `Makefile.tests`:

```bash
cd rvv/ && make -f Makefile.tests
```

Key binaries and their checkers:

| Binary | Checker | Purpose |
|---|---|---|
| `rvv_ara_latency_test.bin` | `check_ara_latencies.py` | Per-instruction chaining latency for all VFU categories |
| `rvv_load_chain_test.bin` | `run_load_chain_test.py` | Load-to-compute chaining with/without `--enable-chaining` |
| `rvv_chaining_test.bin` | — | Compute-to-compute chaining smoke test |

gem5's own test infrastructure (see `TESTING.md`):

```bash
scons build/ALL/unittests.opt                          # all C++ unit tests
scons build/ALL/base/bitunion.test.opt && \
  ./build/ALL/base/bitunion.test.opt                   # single test binary
./build/ALL/base/bitunion.test.opt --gtest_filter=X    # single test case
./build/ALL/gem5.opt tests/run_pyunit.py               # Python unit tests
```

## Architecture: where the ARA model lives

The model is **modular** — a `LatencyModel` SimObject plugs into the CPU and the RISC-V vector ISA queries it. To add a new architecture's latency table you write Python only; you should not need to touch C++.

| File | Role |
|---|---|
| `src/cpu/LatencyModel.py` | Base SimObject for pluggable latency providers |
| `src/cpu/latency_model.hh/.cc` | C++ strategy implementation |
| `src/cpu/static_inst.hh` | `dynamicOpLatency` virtual dispatch point |
| `src/arch/riscv/insts/vector.hh` | Vector insts call into the CPU's `latencyModel`; throughput formula lives here |
| `src/cpu/o3/AraConfig.py` | `AraLatencyModel` definition, reconciled latency values, FU pool split |
| `src/cpu/o3/inst_queue.cc` | O3 issue-stage hook for `dynamicOpLatency` + `WakeDependents` |
| `src/cpu/o3/lsq_unit.hh/.cc` | `VectorLoadChainEvent` (1-cycle VRF-write delay for load chaining) |

The total instruction-to-instruction dependency delay is:

```
total = 1 (issue) + latencyModel->getLatency(opClass, vsew)
```

`AraLatencyModel` reconciles gem5 with RTL by baking in a 7-cycle sequencer dispatch floor and the iterative compute times measured from `ara_pkg.sv`.

## Upstream gem5 layout (orientation only)

- `src/` — C++ source + Python SimObject definitions (`.py` next to `.cc/.hh`)
- `configs/` — Python config scripts; `configs/example/` and `src/python/gem5/` for the standard library
- `build/<ISA>/` — scons output; the binary you almost always want is `build/RISCV/gem5.opt`
- `rvv/` — this fork's ARA test programs, config scripts, and checkers
- `ext/`, `system/`, `util/` — third-party code, full-system disk/kernel assets, helper utilities
