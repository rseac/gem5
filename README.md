# The gem5 Simulator (ARA Vector Timing Model Extension)

This repository is a modified fork of [gem5](https://www.gem5.org/) that incorporates dynamic operation latency bounds mimicking the **ARA RISC-V Vector Architecture**.

## Vanilla gem5 vs. ARA Timing Model

In the default gem5 repository, functional unit latencies (`opLat`) are strictly static. When creating a CPU model config (like `O3CPU` or `MinorCPU`), functional units are assigned a generic, unchanging latency for classes of operations (e.g. `SimdFloatAdd` = 5 cycles) regardless of how much work that operation actually performs.

**The ARA Modification:**
Pipelined Vector Hardware processors process a finite amount of data per clock tick (e.g. one 64-bit element per lane per cycle). That means the time it takes an instruction to leave a functional pipeline depends *entirely* on the size of the elements passing through it. 

To model this, we introduced the `dynamicOpLatency` interface directly into gem5's `StaticInst` layer and hooked it into the Vector (`vtype`) CSRs.

### How it Works
When the O3 or Minor CPU schedulers fetch an instruction, they intercept the standard latency fetch and instead dynamically evaluate:
1.  **SEW (Standard Element Width):** Floating point calculations scale latency dynamically according to the element size requested (`vsew + 2`). Unpipelined elements like `__rvv_f64` divisions (`SimdDivOp`) take significantly longer (`4 << vsew`) than 16-bit half-precision calculations.
2.  **LMUL (Length Multiplier):** We rely on gem5's decoder which automatically splits `LMUL > 1` macro instructions into `num_microops` based on the grouping limits. These micro-ops individually pass through the functional units incurring the `dynamicOpLatency`, scaling the structural hazards and pipeline throughput completely organically.

### Codebase Changes
If you wish to examine the core modifications that enable this dynamic vector latency, see the following files:
*   `src/cpu/static_inst.hh`: The `dynamicOpLatency` virtual dispatch interface.
*   `src/arch/riscv/insts/vector.hh`: The RISC-V overriding logic evaluating the `ThreadContext->PCState` for SEW bounds.
*   `src/cpu/minor/execute.cc`: MinorCPU issue-stage intersection.
*   `src/cpu/o3/inst_queue.cc`: O3CPU dependency calculation intersection.

In addition to dynamic execution latency logic, ARA-specific hardware configuration models have been built in Python to parameterize the O3 and Minor simulators:
*   `src/cpu/o3/AraConfig.py`: Defines the execution units and default latency mappings for the ARA Out-of-Order model (`AraO3CPU`). It configures an `AraFUPool` allocating multiple specialized subunits natively handling the `issueLat` and structural limits. For example, it assigns `count=4` to general `AraSIMD_Unit` blocks to mimic the 4-lane hardware boundaries.
*   `src/cpu/minor/AraMinorConfig.py`: Defines the execution units and default latency mappings for the ARA In-Order model (`AraMinorCPU`). It instantiates localized `MinorOpClassSet` groups representing logical functions (like `AraMinorIntDivVectorFU` which strictly processes `SimdDiv` Operations), allocating their baseline latency parameter structures.

## Running ARA Benchmarks
You can execute RISC-V vector binaries under these ARA-modeled processors using the provided testing configuration scripts inside the `rvv/` folder.

**Simulation Script (`rvv/riscv-rvv-se-ara.py`) Arguments:**
The execution script supports several configuration parameters for tailoring the simulated ARA environment:
*   `resource`: (Positional) The compiled RISC-V binary to execute.
*   `--cpu-type`: System CPU model to use (`AraO3` or `AraMinor`). Defaults to `AraO3`.
*   `-v, --vlen`: Vector Length (VLEN) in bits. Defaults to `256`.
*   `-e, --elen`: Vector Extension Length (ELEN) in bits. Defaults to `64`.
*   `-c, --cores`: Number of cores to simulate. Defaults to `1`.
*   `-d, --l1d`: Size of the Level 1 Data Cache. Defaults to `32KiB`.
*   `-2, --l2`: Size of the Level 2 Cache. Defaults to `512KiB`.
*   `-p, --parms`: Execution arguments to pass to the running binary (e.g., matrix elements). Defaults to `2048`.


Example:
```bash
./build/RISCV/gem5.opt rvv/riscv-rvv-se-ara.py rvv/rvv_arith_latency_64.bin --cpu-type AraMinor
```

For more details on crafting and verifying tests that empirically highlight the pipeline depths scaling natively with Element Widths (SEW), see the testing documentation at [rvv/README.md](rvv/README.md).

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
