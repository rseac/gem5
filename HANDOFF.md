# Ara coprocessor + RiVEC benchmark handoff

Session summary for picking up the gem5 `AraCoprocessor` work. Nothing here
is committed to git - all changes are uncommitted in the working tree
(`git status`/`git diff` shows everything). No background processes left
running.

## What's done

### `AraCoprocessor` (`src/cpu/ara/ara_coprocessor.{cc,hh}`)

Went from a skeleton that deadlocked on the second instruction to something
that runs real benchmarks correctly. Bugs found and fixed, in order:

1. **Memory completion was never signaled back to the CPU** -
   `handleMemoryResponse()` marked a hardcoded mock register ready instead
   of the real destination, and never called `markCompleted()` on the real
   instruction. Any vector load/store deadlocked the CPU forever.
2. **Self-dependent instructions blocked on themselves** - an instruction
   reading its own destination as a source (e.g. `vsetvli x0,x0`) had that
   register marked "pending" before it could ever be read, so it waited on
   itself forever.
3. **`sendTimingReq()` failures were silently dropped instead of retried** -
   marked "Retry omitted for brevity" in the original code. When the port
   declined a request, the instruction just vanished; the CPU hung waiting
   for a completion that would never come. Added proper `recvReqRetry()`
   handling that holds the packet and resends it.
4. **Null-pointer segfault on fault instructions** - `inst->staticInst->
   isVector()` was called unguarded in `Execute::issue()`/`commit()`
   (`src/cpu/minor/execute.cc`). MinorCPU issues fault-carrying instructions
   into functional units the same as real ones, and a fault instruction's
   `staticInst` can be null. Guarded with `!inst->isFault()`, matching the
   pattern already used elsewhere in that file.
5. **Scoreboard hazard gap** - vector instructions handled by
   `AraCoprocessor` need to be marked "unpredictable" in MinorCPU's
   scoreboard (`markupInstDests(..., mark_unpredictable=true)`), the same
   way real variable-latency memory refs already are - otherwise dependent
   scalar instructions are allowed to issue using the FU's short static
   latency instead of waiting for Ara's real (much longer, chained)
   completion, reading stale/garbage register state. This was corrupting
   registers like `ra` and causing wild-address crashes on `jr ra`.
6. **Instruction identity was tracked by `StaticInstPtr`** - gem5 caches/
   shares one `StaticInst` object across *every* dynamic occurrence of the
   same instruction (e.g. every loop iteration of the same `vle64.v`), so
   `StaticInstPtr` identity alone can't tell two in-flight loop iterations
   apart. Completion signals could cross-talk between iterations. Now keyed
   on the CPU's unique per-dynamic-instance id
   (`MinorDynInst::id.execSeqNum`), threaded through `pushInstruction()`,
   `hasCompleted()`, `markCompleted()`, `InFlightInst`, and `MemSenderState`.
7. **Out-of-bounds mock-VRF access** - `mock_srcA/srcB/dest` are derived
   heuristically from raw `StaticInst` reg-class indices and aren't
   guaranteed to fall inside the model's fixed 0..31 vector-register slots.
   Added bounds checking to `VectorRegisterFile::markPending/markReady/
   isReady` instead of indexing off the end of the vector (segfault).
8. **`VectorLane::tick()` only marked one byte "ready" per chunk** instead
   of the whole `datapathBytes`-wide chunk actually produced. Most of every
   register stayed permanently "pending" after its producer finished,
   silently deadlocking any later instruction whose dependency check
   happened to probe one of those never-marked bytes. This is the most
   likely explanation for the `pathfinder`/`streamcluster`/`particlefilter`
   hangs described below - **I found and fixed this last and did not
   fully re-verify it closed out all three before running out of budget.
   Check this first.**

### Wrong binaries entirely (fixed)

`apps_gem5/` benchmark binaries were compiled with Ara's own bare-metal RTL
testbench toolchain: custom `crt0`, no real stack, `baremetal_malloc`,
compile-time-embedded `data.S` inputs. Running those ELFs under gem5 SE
mode is guaranteed to crash regardless of coprocessor correctness (no
stack, garbage addresses, manual memory-mapping hacks in the old
`run_gem5_ara.py`).

Fixed by recompiling the *same* RiVEC sources
(`apps_gem5/riscv-vectorized-benchmark-suite/`) with the gem5-ara docker
image's own toolchain (`riscv64-unknown-linux-gnu-gcc`/`-g++` at
`/riscv/_install/bin` inside `rseac/gem5-ara:latest`) instead, producing
normal glibc-linked binaries that SE mode understands natively (automatic
stack/heap/argv). See **`apps_gem5/se_build/README.md`** for the exact
compile recipe per benchmark and the shim
(`apps_gem5/se_build/shim/`) that replaces the handful of bare-metal-only
symbols (`baremetal_malloc`, the custom `printf.h`) with real libc
equivalents. Everything else (the `rdcycle`-based `runtime.h` timer, the
raw RVV inline-asm kernels) was already portable as-is.

Compiled binaries: `apps_gem5/bin/*_se.exe`.

`run_gem5_ara.py` was extended to forward extra CLI args to the simulated
program: `build/RISCV/gem5.opt run_gem5_ara.py <binary.exe> [args...]`
(needed for `_matmul`, `_particlefilter`, `_somier`, `_swaptions`).

### VLEN mismatch (found last, high-impact, needs re-verification)

The real architectural VLEN (`system.cpu.isa[0].vlen`, which governs how
many elements a real `vsetvli` actually processes) was **never set** and
silently defaulted to gem5's built-in default of 256 bits
(`src/arch/riscv/RiscvISA.py`). The RTL baseline directory is literally
named `results_4L_4096V` - **VLEN=4096**. That's a 16x mismatch: every
benchmark was running 16x more vector-loop iterations than it should have.

Note this is a *different* parameter from `AraCoprocessor(vlen=4096)` in
`run_gem5_ara.py` - that one only sizes the coprocessor's own internal mock
register file and was already set correctly; it never propagated to the
real CPU ISA's architectural VLEN.

Fixed in `run_gem5_ara.py` (pure Python config change, no C++ rebuild
needed):

```python
system.cpu.isa = [RiscvISA(vlen=4096, elen=64)]
```

**This was mid-verification when the session ended.** `axpy_se.exe` was
re-run with the fix; a debug trace (`--debug-flags=AraCoprocessor`)
confirmed it was actively progressing (issuing instructions, completing
memory ops, ticking normally - not stuck), but the run hadn't finished
before time ran out. Re-run it and compare against the RTL baseline.

### `collect_mape.py` (fixed)

Was passing host-absolute paths (e.g.
`/home/twiga/.../gem5/apps_gem5/bin/_axpy`) as the argument to
`run_gem5_ara.py` *inside* the docker container, where only `/gem5`
(mounted from `$(pwd)`) exists - so every benchmark failed to find its own
executable and returned 0 cycles instantly. This is why an early full-batch
run looked like it "completed fast" with all-zero results - it wasn't
actually running anything. Fixed to pass a path relative to `/gem5`.

Also update the `BENCHMARKS`/binary names in `collect_mape.py` to point at
the new `apps_gem5/bin/*_se.exe` binaries instead of the old bare-metal
ones, and update `run_gem5_ara.py`'s VLEN fix is picked up (it will be,
since `collect_mape.py` just shells out to `run_gem5_ara.py`).

## Benchmark status

**Confirmed working before the VLEN fix** (compiled, ran to completion,
passed internal verification): `axpy`, `blackscholes`, `matmul`,
`jacobi2d`, `lavaMD`, `swaptions`, `somier`. Cycle counts were ~10-20x over
the RTL baseline's `ROI_Cycles`
(`/home/twiga/code/github/ai/my-timing-project/benchmark_suite/ara/results_4L_4096V/status.csv`)
- expected given `AraCoprocessor`'s timing model is a rough approximation,
but the VLEN fix above should shrink this gap substantially since it was
inflating trip counts 16x. **Needs re-verification against RTL post-VLEN-fix.**

**Still broken, two distinct root causes:**

- **`spmv`** crashes almost immediately (garbage computed address, `Page
  table fault`). It's the first benchmark using an indexed/gather load
  (`x[ja[i]]`, `vluxei64`-style). Real RVV gather instructions likely
  decompose into multiple per-element micro-ops in gem5's ISA
  implementation, and `AraCoprocessor`'s model only tracks one destination
  register per instruction, not per-lane/per-element. This needs real
  design work, not a quick patch - not attempted.

- **`pathfinder`/`streamcluster`/`particlefilter`** hang partway through
  (not in the benchmark's own code - after it, in what looks like glibc
  library code, instruction `vmv_v_i_micro`, likely a vectorized `memset`).
  Debug trace showed a functional unit (`FU: 4`) permanently "stalled" -
  an earlier vector instruction stuck in it forever, waiting on
  `AraCoprocessor` completion that never arrives. Bug #8 above
  (`markReady` only marking one byte instead of a whole chunk) is the most
  likely cause and was fixed, but **not re-verified to close out these
  three** before the session ended - check this first before assuming
  further investigation is needed.

## Immediate next steps

1. Finish verifying the VLEN=4096 fix: re-run `axpy_se.exe` via
   `run_gem5_ara.py`, confirm it completes, compare cycle count against the
   RTL baseline.
2. Re-test `pathfinder`/`streamcluster`/`particlefilter` against the
   `markReady` byte-range fix (#8) - may already be resolved.
3. Once (1) and (2) look good, run `collect_mape.py` for a real MAPE
   comparison across all working benchmarks.
4. `spmv`'s gather-load gap is the one open design question, separate from
   everything else above.

## Useful commands

Build (from `/home/twiga/code/github/ai/tunable-gem5/gem5`):
```bash
docker run --rm -u $(id -u):$(id -g) -v "$(pwd)":/gem5 -w /gem5 \
  rseac/gem5-ara:latest scons build/RISCV/gem5.opt -j4
```

Run a benchmark:
```bash
docker run --rm -u $(id -u):$(id -g) -v "$(pwd)":/gem5 -w /gem5 \
  rseac/gem5-ara:latest build/RISCV/gem5.opt run_gem5_ara.py \
  apps_gem5/bin/axpy_se.exe
```

Debug trace (narrow the window with `--debug-start=<tick>` - full traces
from tick 0 are enormous):
```bash
... build/RISCV/gem5.opt --debug-flags=AraCoprocessor,MinorExecute \
  --debug-start=<tick> run_gem5_ara.py apps_gem5/bin/<name>_se.exe
```

Note: `docker run --rm ... | timeout N` does not reliably kill the
underlying container when the timeout fires - it kills the `docker run`
client but the container can keep running server-side. Use
`docker ps`/`docker kill <id>` to clean up stray containers if a run
seems to hang past its timeout.
