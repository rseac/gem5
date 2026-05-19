# TSVC 2

## Compiling

Requires the RISC-V bare-metal toolchain at `/riscv/_install/bin/`.

```bash
cd src
make
```

This produces three binaries under `bin/GNU/`, each compiled with different
vectorisation settings, together with their disassemblies (`.dump`):

| Binary | Vectorisation |
|---|---|
| `tsvc_vec` | enabled (`-ftree-vectorize`) |
| `tsvc_novec` | disabled (`-fno-tree-vectorize`) |
| `tsvc_noScalvec` | no scalar vectorisation |

### Compile-time kernel subset

To bake a specific subset of kernels into the binary (no runtime arguments
needed — useful for gem5):

```bash
make KERNELS="s000"          # single kernel
make KERNELS="s000 s111 va"  # multiple kernels
```

### Optional flags

| Flag | Effect |
|---|---|
| `NO_M5OPS=1` | Disable gem5 `m5_reset_stats`/`m5_dump_reset_stats` ROI markers (enabled by default, requires `libm5.a` — see below) |
| `VEC_REPORT=1` | Print compiler vectorisation diagnostics |
| `FAST_MATH=1` | Enable `-ffast-math` |

#### Building libm5 (required — ROI is on by default)

```bash
cd /gem5/util/m5
scons build/riscv/out/m5
cd -
make
```

## Running under gem5 with the ARA timing model

```bash
/gem5/build/RISCV/gem5.opt riscv-rvv-se-ara.py ./bin/GNU/tsvc_vec \
    --cpu-type AraO3                 \
    --vlen 4096                      \
    --simd-units 2                   \
    --vector-timing-throughput 4     \
    --enable-chaining
```

### Key options

| Option | Description |
|---|---|
| `--cpu-type AraO3` | Use the ARA out-of-order core model |
| `--vlen 4096` | Vector register length in bits |
| `--simd-units N` | Number of SIMD lanes |
| `--vector-timing-throughput N` | Issue throughput for vector instructions |
| `--enable-chaining` | Enable vector instruction chaining |

To run only a specific kernel, compile with `KERNELS="s000"` (see above) and
use the resulting binary directly — no extra arguments are needed at run time.

## Output

Each kernel prints a performance line to the terminal:

```
Loop                	      Cycles	Checksum
s000                	      142378	471923584.000000
```

- **Cycles** — RISC-V `rdcycle` delta, reflects simulated cycles from the ARA timing model.
- **Checksum** — Correctness verification value, automatically validated against official golden data in the automated runs.

---

## Automated Subset Execution & Scaling

A dedicated script, `run-subset.sh`, is provided to automate the execution of a specific subset of kernels across multiple iteration counts, collect all relevant data, and generate a summary CSV.

### Subset Kernels
The current subset includes: `s000`, `s121`, `s131`, `s162`, `s171`, `s211`, `s243`, `s251`, `s128`, `s311`, `s341`.

### Running in Docker
To execute the full automated suite inside the `jovial_curran` container:

```bash
docker exec -it -w /gem5/tsvc-gem5 jovial_curran ./run-subset.sh
```

### What the script does:
1.  **Iteration Scaling:** Automatically iterates through `1, 5, 10, and 20` iterations for every kernel.
2.  **Wallclock Timing:** Measures the real-world execution time (Host Wallclock) for each simulation run.
3.  **Data Organization:** Creates a timestamped results directory (e.g., `subset_results_YYYYMMDD_HHMMSS/`) with subdirectories for each kernel and iteration count.
4.  **Metric Collection:** Captures:
    *   **Arch Cycles:** Architectural hardware cycles from the simulation.
    *   **ROI Cycles (Gem5):** Internal gem5 CPU cycles for the Region of Interest.
    *   **ROI Vector Insts:** Number of committed vector instructions in the ROI.
    *   **Checksums:** The calculated checksum for correctness.
5.  **Validation:** Automatically compares every checksum against the official reference data in `input-small/`.
6.  **CSV Generation:** Produces a consolidated `summary_results.csv` inside the results folder containing all the above metrics and a **PASS/FAIL** validation status.

---

## About

Updated version of TSVC to capture the same information but hopefully in a better format to make it easier to add things. We've fixed a few bugs along the way too.

The original paper which laid out the details of the suite and provided results (D. Callahan, J. Dongarra, and D. Levine. Vectorizing compilers: a test suite and results. Proceedings. Supercomputing '88) originally had the loops in Fortran.

The C version of the benchmark used as a base for TSVC-2 was found [here](http://polaris.cs.uiuc.edu/~maleki1/TSVC.tar.gz). The old C version had some problems to do with compilers being able to inline all of the initialisation and checksum loops, which wildly skews the results and the timing. There are also some bugs with writing off the end of arrays and some of the initialisation routines seem to not initialise the right arrays.

This version should (hopefully) be free(er) of bugs and should stop the compiler from being able to completely eliminate code.

There are a few supplied makefiles for each compiler so just typing `make` along with specifying the compiler should work. The flags in these makefiles are generally used flags such as `-O3`, with some compilers having a few extra flags which help a lot with vectorisation.

For example, with the Cray compiler:

    make COMPILER=cray

Some compilers (GNU, clang) do not have a 'precise' flag, and the PGI compiler will ignore the IEEE math flag with vectorisation enabled.

*NB - none of the supplied makefiles have any architecture-specific flags and you will need to edit the makefiles to supply them*.

If you find any issues or have any requests, please let us know!
