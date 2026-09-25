# SE-mode RiVEC builds

Handoff notes for continuing the gem5-ara coprocessor work. See the chat
session for full context; this covers reproducing the build.

## Why this exists

`apps_gem5/riscv-vectorized-benchmark-suite/` is ported to Ara's own
bare-metal RTL testbench runtime (custom crt0, `baremetal_malloc`, embedded
`data.S` inputs, no real stack). Running those bare-metal ELFs under gem5 SE
mode crashes (no stack, garbage addresses) because SE mode expects a normal
glibc-linked binary. The fix: compile the *same* RiVEC sources with the
gem5-ara docker image's own toolchain (`riscv64-unknown-linux-gnu-gcc`,
found at `/riscv/_install/bin` in `rseac/gem5-ara:latest`) instead, so the
binary gets a real stack/heap/argv via glibc, which SE mode understands
natively.

`shim/` replaces only the handful of bare-metal-only symbols
(`baremetal_malloc`, the custom `printf.h`) with real libc equivalents.
Everything else (the `runtime.h` cycle-counter, the raw RVV inline asm
kernels) is already portable as-is.

## Compile command shape (run inside the docker container)

```bash
SUITE=/gem5/apps_gem5/riscv-vectorized-benchmark-suite
CC=riscv64-unknown-linux-gnu-gcc   # or ...-g++ for .cpp sources
IFLAGS="-I /gem5/apps_gem5/se_build/shim -I /gem5/apps_gem5/common -I $SUITE"
DEFS="-DUSE_RISCV_VECTOR -DNR_LANES=4 -DNR_CLUSTERS=1"
$CC -march=rv64gcv $DEFS -static -O2 -fno-tree-vectorize $IFLAGS \
  <benchmark sources...> [<benchmark>_data.S] shim/compat_globals.c \
  -o apps_gem5/bin/<name>_se.exe -lm
```

Compiled binaries already live at `apps_gem5/bin/*_se.exe`. To rebuild from
scratch, see the exact per-benchmark invocations in the chat session (each
benchmark needed slightly different source files - check
`_<name>/Makefile`'s `vector:` target for the current file list, since it
may be stale/renamed vs. what's actually in `src/`).

### Benchmarks needing embedded data (`*_data.S`, generated via each
### benchmark's `script/gen_data.py`, already committed here)

- `_axpy`: no script; `shim/compat_axpy.c` just defines `int n = 4096;`
  (matches RTL baseline's `args_app=4096`).
- `_blackscholes`: `gen_data.py input/in_8.input`
- `_pathfinder`: `gen_data.py input/data_small.in`
- `_swaptions`: `gen_data.py 128 1`
- `_lavaMD`: `gen_data.py 1 1 32`
- `_spmv`: `gen_data.py input/football.mtx input/football.verif`
- `_streamcluster`: `gen_data.py 3 3 128 8 8 10`

### Benchmarks using real argv/file I/O (no data.S needed)

- `_matmul <input file>` - use e.g. `_matmul/input/data_64.in`
- `_particlefilter -x 128 -y 128 -z 2 -np 256`
- `_somier 5 10`
- `_jacobi-2d` - fully self-contained (static `MAX_N=64` arrays), no args
- `_swaptions` also needs **any** dummy CLI arg (e.g. `-run`) at runtime -
  argc==1 alone triggers its usage-and-exit path, even though the actual
  values come from the data.S globals, not argv parsing.

`run_gem5_ara.py` was extended to forward extra argv:
`build/RISCV/gem5.opt run_gem5_ara.py <binary.exe> [args...]`.
