# Apps

This folder contains the benchmarks, programs, and tests ready to be run on AraXL.
All software sources are licensed under [Apache 2.0](../LICENSE.sw).

SW utilities and benchmarks for AraXL using external contributions,
* `RiVEC` - Use of the RISC-V VECTOR intrinsics mapping `common/rivec/vector_defines.h` and benchmarks `apps/cos`, `apps/log`, `apps/exp` by Cristóbal Ramírez Lazo, "Barcelona 2019" under [license](common/rivec/LICENSE)
* `print` - `common/printf.c`, `common/printf.h` Print primitives for embedded systems under the MIT License (MIT)
* `common/encoding.h` - Copyright (c) 2010-2017, The Regents of the University of California

* `dwt` - Scalar version ported to RVV environment for Ara
* `jacobi2d` - Modified vector version from PolyBench/C and then RiVEC benchmark suites
* `pathfinder` - Modified vector version from RODINIA and then RiVEC benchmark suites
* `roi_align` - Porting to RVV environment for Ara from the Original implementation taken from https://github.com/longcw/RoIAlign.pytorch
* `softmax` - Scalar implmentation inspired by OpenCV softmax https://github.com/opencv/opencv/blob/master/modules/dnn/src/layers/softmax_layer.cpp

### RiVEC benchmarks

RiVEC benchmarks have been ported to the AraXL baremetal execution environment added as a gitsubmodule to this repository,

To compile the rivec benchmark,
In the apps folder,
```bash
make rivec_binaries nr_clusters=4
```

To run the rivec benchmark, from the hardware folder run the rivec binary as usual
```bash
make sim app=rivec-binary nr_clusters=4
```

### Hello World
Run the following command to build an application. E.g., `hello_world`:

```bash
cd apps
make bin/hello_world
```

### Convolutions

Convolutions allow to specify the output matrix size and the size of the filter, with the variables `OUT_MTX_SIZE` up to 112 and `F_SIZE` within {3, 5, 7}. Currently, not all the configurations are supported for all the convolutions. For more information, check the `main.c` file for the convolution of interest.
Example:

```bash
cd apps
make bin/fconv2d OUT_MTX_SIZE=112 F_SIZE=7
```

### Standard RISC-V tests

To compile the standardized riscv tests from https://github.com/riscv/riscv-tests
```bash
cd apps
make riscv_tests_compile
```

### RISC-V vector tests
To compile and run handwritten test cases for RISC-V vector ISA
```bash
cd apps
make riscv_tests
```

### Running under gem5 (AraCoprocessor timing model)

Everything above targets the AraXL baremetal/RTL (Verilator) hardware
simulation flow. This repo also carries a gem5-side, SE-mode (syscall
emulation) analytical timing model of Ara, `AraCoprocessor`
(`src/cpu/ara/ara_coprocessor.{cc,hh}`), attached to `MinorCPU`. It's an
approximate, fast model rather than a cycle-exact RTL replica - the point is
to run full RiVEC benchmarks in gem5 far faster than Verilator, at the cost
of some timing accuracy.

**Build gem5** (from the repo root, via the project's Docker image):
```bash
docker run --rm -u $(id -u):$(id -g) -v "$(pwd)":/gem5 -w /gem5 \
    rseac/gem5-ara:latest scons build/RISCV/gem5.opt -j4
```

**Build the benchmarks for gem5 SE mode**: the compiled `*_se.exe` binaries
already checked in under `apps_gem5/bin/` are gitignored (build artifacts);
rebuild them from the `riscv-vectorized-benchmark-suite/` sources the same
way the RTL flow does, targeting the gem5 SE-mode toolchain rather than the
baremetal one.

**Run a benchmark**:
```bash
docker run --rm -u $(id -u):$(id -g) -v "$(pwd)":/gem5 -w /gem5 \
    rseac/gem5-ara:latest build/RISCV/gem5.opt run_gem5_ara.py \
    apps_gem5/bin/matmul_se.exe apps_gem5/riscv-vectorized-benchmark-suite/_matmul/input/data_64.in
```
`run_gem5_ara.py` configures a single-issue, in-order `MinorCPU` tuned to
match CVA6, with the `AraCoprocessor` attached (default `num_lanes=4`,
`vlen=4096`). Each benchmark prints `[ROI-LATENCY]: N cycles` for its
timed region.

**Current accuracy** (MAPE against a matching Ara RTL/Verilator baseline,
`results_4L_4096V/status.csv`), averaged across the 11-benchmark RiVEC
suite: **18.5%**, ranging from 2.0% (pathfinder) to 38.1% (jacobi-2d). See
`HANDOFF.md` for the full benchmark-by-benchmark table, every fix made to
reach this, and two changes that were tried and reverted after regressing
the broader suite despite improving a narrow subset. This work lives on the
`ara-coprocessor-timing-model` branch.