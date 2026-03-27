# The gem5 Simulator (ARA Vector Timing Model Extension)

This repository is a modified fork of [gem5](https://www.gem5.org/) that models the **ARA RISC-V Vector Architecture** timing behaviour, including dynamic operation latency, vector chaining, and accurate load-chaining through the memory hierarchy.

## ARA Timing Model

The model replaces gem5's default 1-cycle vector latencies with values derived from the ARA RTL (`ara_pkg.sv`). Three capabilities not present in upstream gem5 are added:

1. **Dynamic operation latency** — FU occupancy scales with `VLEN`, element width (`SEW`), and lane count (`NrLanes`).
2. **Compute chaining** — a consumer instruction issues as soon as the producer's first result element is available (`WakeDependents` at `chainingLatency` cycles), without waiting for the full vector to complete.
3. **Cache-miss-accurate load chaining** — vector loads participate in chaining based on actual memory latency via a 1-cycle `VectorLoadChainEvent` fired after `completeAcc()`.

### Key Source Files

| File | Purpose |
|------|---------|
| `src/cpu/static_inst.hh` | `dynamicOpLatency` virtual dispatch interface |
| `src/arch/riscv/insts/vector.hh` | `dynamicOpLatency()`, `chainingLatency()`, `DISPATCH_FLOOR` |
| `src/cpu/o3/inst_queue.cc` | O3 issue-stage hook for `dynamicOpLatency` and `WakeDependents` |
| `src/cpu/o3/lsq_unit.hh/.cc` | `VectorLoadChainEvent` — 1-cycle VRF-write delay for load chaining |
| `src/cpu/o3/AraConfig.py` | Split FU pool; calibrated `opLat` values; `pipelined=False` on serial dividers |
| `src/cpu/minor/execute.cc` | MinorCPU issue-stage hook for `dynamicOpLatency` |
| `src/cpu/minor/AraMinorConfig.py` | ARA in-order CPU configuration |

### Latency Formulae

**FU occupancy** (`dynamicOpLatency`):
```
dynamicOpLatency = max(pipeline_depth + ceil(vl / (NrLanes × ELEN/sew)), DISPATCH_FLOOR)
```

**Chaining latency** (when consumer can issue):
```
chainingLatency = max(pipeline_depth + 2, DISPATCH_FLOOR)
```

`DISPATCH_FLOOR = 6` — minimum enforced by ARA's scoreboard.

### Functional Unit Pool (`AraConfig.py`)

The FU pool is split into three classes to correctly model ARA's structural hazards:

| Class | `count` | `pipelined` | Covers |
|-------|---------|-------------|--------|
| `AraSIMD_Pipelined` | `simd_units` (default 2) | True | ALU, Mul, FP compute/compare/convert/reduce, LSU-AGU |
| `AraSIMD_IntDiv` | 1 | False | `SimdDiv` only — ARA's one serial integer divider |
| `AraSIMD_FPDivSqrt` | 1 | False | `SimdFloatDiv`, `SimdFloatSqrt` — fpnew DIVSQRT is iterative |

`AraSIMD_Pipelined` uses `count=2` (not the lane count) because gem5's `WakeDependents` chaining requires one FU slot for the producer and one for the consumer. `AraSIMD_IntDiv` and `AraSIMD_FPDivSqrt` use `count=1` to enforce that two independent divide instructions cannot execute simultaneously.

## Running ARA Simulations

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
| `--vector-timing-throughput` | 4 | **Number of ARA lanes** — scales `NrLanes` in the throughput formula |
| `--simd-units` | 2 | FU pool slots for `AraSIMD_Pipelined` — always keep at 2 |

`--vector-timing-throughput` should match the ARA hardware lane count (2, 4, 8, or 16). `--simd-units` should always be 2 regardless of lane count — see `README_ARA_sim.md` for a full explanation.

## Test Suite

Tests are in `rvv/` and built with `Makefile.tests`:

```bash
cd rvv/
make -f Makefile.tests
```

| Binary | Checker script | What it tests |
|--------|---------------|---------------|
| `rvv_ara_latency_test.bin` | `check_ara_latencies.py` | Per-instruction chaining latency for all ARA VFU categories |
| `rvv_load_chain_test.bin` | `run_load_chain_test.py` | Load-to-compute chaining with and without `--enable-chaining` |
| `rvv_chaining_test.bin` | — | Compute-to-compute chaining smoke test |

For full parameter documentation, latency tables, and chaining activation thresholds, see [README_ARA_sim.md](README_ARA_sim.md).

---

## Original gem5 Repository Information

This is the repository for the gem5 simulator. It contains the full source code
for the simulator and all tests and regressions.

The gem5 simulator is a modular platform for computer-system architecture
research, encompassing system-level architecture as well as processor
microarchitecture. It is primarily used to evaluate new hardware designs,
system software changes, and compile-time and run-time system optimizations.

The main website can be found at <http://www.gem5.org>.

## Testing status

**Note**: These regard tests run on the develop branch of gem5:
<https://github.com/gem5/gem5/tree/develop>.

[![Daily Tests](https://github.com/gem5/gem5/actions/workflows/daily-tests.yaml/badge.svg?branch=develop)](https://github.com/gem5/gem5/actions/workflows/daily-tests.yaml)
[![Weekly Tests](https://github.com/gem5/gem5/actions/workflows/weekly-tests.yaml/badge.svg?branch=develop)](https://github.com/gem5/gem5/actions/workflows/weekly-tests.yaml)
[![Compiler Tests](https://github.com/gem5/gem5/actions/workflows/compiler-tests.yaml/badge.svg?branch=develop)](https://github.com/gem5/gem5/actions/workflows/compiler-tests.yaml)

## Getting started

A good starting point is <http://www.gem5.org/about>, and for
more information about building the simulator and getting started
please see <http://www.gem5.org/documentation> and
<http://www.gem5.org/documentation/learning_gem5/introduction>.

## Building gem5

To build gem5, you will need the following software: g++ or clang,
Python (gem5 links in the Python interpreter), SCons, zlib, m4, and lastly
protobuf if you want trace capture and playback support. Please see
<http://www.gem5.org/documentation/general_docs/building> for more details
concerning the minimum versions of these tools.

Once you have all dependencies resolved, execute
`scons build/ALL/gem5.opt` to build an optimized version of the gem5 binary
(`gem5.opt`) containing all gem5 ISAs. If you only wish to compile gem5 to
include a single ISA, you can replace `ALL` with the name of the ISA. Valid
options include `ARM`, `NULL`, `MIPS`, `POWER`, `RISCV`, `SPARC`, and `X86`
The complete list of options can be found in the build_opts directory.

See https://www.gem5.org/documentation/general_docs/building for more
information on building gem5.

## The Source Tree

The main source tree includes these subdirectories:

* build_opts: pre-made default configurations for gem5
* build_tools: tools used internally by gem5's build process.
* configs: example simulation configuration scripts
* ext: less-common external packages needed to build gem5
* include: include files for use in other programs
* site_scons: modular components of the build system
* src: source code of the gem5 simulator. The C++ source, Python wrappers, and Python standard library are found in this directory.
* system: source for some optional system software for simulated systems
* tests: regression tests
* util: useful utility programs and files

## gem5 Resources

To run full-system simulations, you may need compiled system firmware, kernel
binaries and one or more disk images, depending on gem5's configuration and
what type of workload you're trying to run. Many of these resources can be
obtained from <https://resources.gem5.org>.

More information on gem5 Resources can be found at
<https://www.gem5.org/documentation/general_docs/gem5_resources/>.

## Getting Help, Reporting bugs, and Requesting Features

We provide a variety of channels for users and developers to get help, report
bugs, requests features, or engage in community discussions. Below
are a few of the most common we recommend using.

* **GitHub Discussions**: A GitHub Discussions page. This can be used to start
discussions or ask questions. Available at
<https://github.com/orgs/gem5/discussions>.
* **GitHub Issues**: A GitHub Issues page for reporting bugs or requesting
features. Available at <https://github.com/gem5/gem5/issues>.
* **Jira Issue Tracker**: A Jira Issue Tracker for reporting bugs or requesting
features. Available at <https://gem5.atlassian.net/>.
* **Slack**: A Slack server with a variety of channels for the gem5 community
to engage in a variety of discussions. Please visit
<https://www.gem5.org/join-slack> to join.
* **gem5-users@gem5.org**: A mailing list for users of gem5 to ask questions
or start discussions. To join the mailing list please visit
<https://www.gem5.org/mailing_lists>.
* **gem5-dev@gem5.org**: A mailing list for developers of gem5 to ask questions
or start discussions. To join the mailing list please visit
<https://www.gem5.org/mailing_lists>.

## Contributing to gem5

We hope you enjoy using gem5. When appropriate we advise sharing your
contributions to the project. <https://www.gem5.org/contributing> can help you
get started. Additional information can be found in the CONTRIBUTING.md file.
