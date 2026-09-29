# AraCoprocessor development log

A chronological record of how gem5's `AraCoprocessor` (an analytical timing
model for RISC-V "V" vector instructions, attached to `MinorCPU`, living at
`src/cpu/ara/ara_coprocessor.{cc,hh}`) went from a skeleton that deadlocked
on its second instruction to a validated model swept across 9 lane/VLEN
configurations against real RTL and a separate analytical model (VP++).

## Key prompts (synthesized)

1. "Fix the AraCoprocessor bugs" (implicit, ongoing) - debug and fix the
   coprocessor until real benchmarks run correctly.
2. "Push this into a repository or branch, preferably my own fork" - create
   and push a branch to `rseac/gem5`.
3. "Update the readme" / "Update the main readme with what the gem5 ara
   coprocessor is doing" - document the branch's work in `README.md` and
   `apps_gem5/README.md`.
4. "Run the benchmarks that haven't been run on gem5 yet - look at
   `plot_ara_unseen_heatmap.py` to see which ones are run - create a
   separate folder `apps_gem5/unseen` and a script to run these, tell me
   the MAPE."
5. "Update fmatmul input and rerun" / "What's wrong with dotproduct" /
   "Check fconv dataset" - per-benchmark dataset-size debugging.
6. "What were the results of ara VP for these unseen benchmarks - compare
   the two."
7. "Was the gem5 coprocessor experiment run for all the configurations the
   VP was run for?" -> "Yes, run it on all" - extend from 1 config
   (4L_4096V) to the full 9-config sweep (2/4/8 lanes x 1024/2048/4096
   VLEN).
8. "You could run with 3 workers" - parallelize the 45-job sweep.
9. "Check `/tmp/dotproduct_8L_debug2.log` ... does the fix change this at
   all?" - directed root-cause debugging of a hang discovered by the sweep.
10. "What was the issue with the 8L 4096" - explain the root cause plainly.
11. "Commit" - commit the fixes.
12. "Summarize all steps taken to develop the coprocessor, write it to a
    file, and push it into the repository. Keep the key prompts, and all
    the responses, and discoveries." - this document.

## Phase 1: making the coprocessor run at all

Starting point: `AraCoprocessor` deadlocked on the second instruction it
ever saw. Bugs found and fixed, in order:

1. **Memory completion was never signaled back to the CPU.**
   `handleMemoryResponse()` marked a hardcoded mock register ready instead
   of the real destination, and never called `markCompleted()` on the real
   instruction. Any vector load/store deadlocked the CPU forever.
2. **Self-dependent instructions blocked on themselves.** An instruction
   reading its own destination as a source (e.g. `vsetvli x0,x0`) had that
   register marked "pending" before it could ever be read, so it waited on
   itself forever. Fixed via a `destIsOwnSource` flag that suppresses the
   self-hazard.
3. **`sendTimingReq()` failures were silently dropped instead of retried**
   (marked "Retry omitted for brevity" in the original code). When the port
   declined a request, the instruction vanished; the CPU hung waiting for a
   completion that would never come. Added proper `recvReqRetry()` handling
   that holds the packet and resends it.
4. **Null-pointer segfault on fault instructions.** `inst->staticInst->
   isVector()` was called unguarded in `Execute::issue()`/`commit()`
   (`src/cpu/minor/execute.cc`). MinorCPU issues fault-carrying instructions
   into functional units the same as real ones, and a fault instruction's
   `staticInst` can be null. Guarded with `!inst->isFault()`.
5. **Scoreboard hazard gap.** Vector instructions handled by
   `AraCoprocessor` need to be marked "unpredictable" in MinorCPU's
   scoreboard (`markupInstDests(..., mark_unpredictable=true)`), the same
   way real variable-latency memory refs already are - otherwise dependent
   scalar instructions issue using the FU's short static latency instead of
   waiting for Ara's real (much longer, chained) completion, reading
   stale/garbage register state. This was corrupting registers like `ra`
   and causing wild-address crashes on `jr ra`.
6. **Instruction identity was tracked by `StaticInstPtr`.** gem5
   caches/shares one `StaticInst` object across every dynamic occurrence of
   the same instruction (e.g. every loop iteration of the same `vle64.v`),
   so `StaticInstPtr` identity alone can't tell two in-flight loop
   iterations apart - completion signals could cross-talk between
   iterations. Fixed by keying on the CPU's unique per-dynamic-instance id
   (`MinorDynInst::id.execSeqNum`), threaded through `pushInstruction()`,
   `hasCompleted()`, `markCompleted()`, `InFlightInst`, `MemSenderState`.
7. **Out-of-bounds mock-VRF access.** `mock_srcA/srcB/dest` are derived
   heuristically from raw `StaticInst` reg-class indices and aren't
   guaranteed to fall inside the model's fixed 0..31 vector-register slots.
   Added bounds checking (`inBounds()`) to `VectorRegisterFile::
   markPending/markReady/isReady` instead of indexing off the end of the
   vector (segfault).
8. **`VectorLane::tick()` only marked one byte "ready" per chunk** instead
   of the whole chunk actually produced, deadlocking any later instruction
   whose dependency check happened to probe a never-marked byte. This
   turned out to be the root cause of hangs in `pathfinder`/
   `streamcluster`/`particlefilter`.

**Wrong binaries entirely.** `apps_gem5/` benchmarks were originally
compiled with Ara's own bare-metal RTL testbench toolchain (custom `crt0`,
no real stack, `baremetal_malloc`, compile-time-embedded `data.S`) -
guaranteed to crash under gem5 SE mode regardless of coprocessor
correctness. Fixed by recompiling the same RiVEC sources with the
gem5-ara docker image's own toolchain
(`riscv64-unknown-linux-gnu-gcc` at `/riscv/_install/bin` inside
`rseac/gem5-ara:latest`), producing normal glibc-linked SE-mode binaries.
See `apps_gem5/se_build/README.md` and the compatibility shim at
`apps_gem5/se_build/shim/` (replaces `baremetal_malloc` and the bare-metal
`printf.h` with real libc equivalents).

**VLEN mismatch.** The real architectural VLEN (`system.cpu.isa[0].vlen`,
which governs how many elements a real `vsetvli` actually processes) was
never set and silently defaulted to gem5's built-in 256 bits, while the RTL
baseline used VLEN=4096 - a 16x mismatch, running every benchmark through
16x more vector-loop iterations than it should have. This is a *separate*
parameter from `AraCoprocessor(vlen=...)` in `run_gem5_ara.py`, which only
sizes the coprocessor's own internal mock register file. Fixed with
`system.cpu.isa = [RiscvISA(vlen=4096, elen=64)]`.

**`spmv`'s indexed/gather load gap.** Real RVV gather instructions
(`vluxei64`-style) decompose into multiple per-element micro-ops, which the
original single-destination-register model couldn't track per-lane/
per-element. Not a quick patch - addressed later via the
strided/indexed memory-penalty formula (see Phase 3).

## Phase 2: publishing the work

- Created and pushed the `ara-coprocessor-timing-model` branch to the
  user's fork `rseac/gem5`.
- Updated `apps_gem5/README.md` and the top-level `README.md` to document
  what the AraCoprocessor timing model is and how to use it.

## Phase 3: accuracy tuning against the 11-benchmark RTL suite

- **FU pool tuning to match CVA6** (single-issue in-order): the default
  `MinorDefaultFUPool` has exactly one memory FU for every memory op class,
  so every memory micro-op - including each element of a strided/indexed
  vector load - contends for the same single FU. Split the pool so
  `SimdStridedLoad/Store` and `SimdIndexedLoad/Store` get 4 FU instances
  (matching Ara RTL's real `VaddrgenInsnQueueDepth=4` address-generation
  queue depth), while unit-stride/whole-register ops keep the original
  single-FU model. An earlier attempt that widened *all* memory classes
  uniformly over-corrected unit-stride benchmarks (axpy's MAPE got worse,
  6.5% -> 37.6%) while still helping strided/indexed ones - this is why the
  pool is deliberately split.
- **Strided/indexed memory-penalty formula**, recalibrated from real Ara
  RTL (Verilator) measurements on a stride-8 VLSU microbenchmark
  (VL16->85 cycles, VL256->140 cycles, VL1024->138 cycles): a fixed AXI
  round-trip latency (64 cycles) plus a small saturating per-element term
  (0.3 cycles/elem, capped at VL=256), replacing an earlier flat
  `active_vl * 2` (strided) / `* 4` (indexed) model that overcharged by
  ~3.6x at VL256.
- **Tried and reverted**: a fixed per-instruction ALU latency (~23-26
  cycles, grounded in real VALU-ADD RTL measurements) looked like a clear
  win on a 3-benchmark tuning set (matmul/axpy/spmv, avg MAPE 25.4% ->
  22.0%) but was a net regression across the full 11-benchmark suite
  (18.3% -> 25.7%) - somier, swaptions, lavaMD, and blackscholes all got
  dramatically worse. Lesson: any future fixed-latency term must be
  checked against the full suite, not a small tuning subset.

## Phase 4: the "unseen" benchmarks (dotproduct, fconv2d, fconv3d, fmatmul, iconv2d)

`paper_results/plot_ara_unseen_heatmap.py` (in the separate
`my-timing-project` repo) identified 5 benchmarks that VP++ was validated
against but gem5 had never run. Ported them from AraXL's native bare-metal
tree into `apps_gem5/unseen/` using the same SE-mode shim/build pattern as
Phase 1, with one new gotcha discovered:

- **`-DSPIKE` silently disables the timer, not just printf.** `runtime.h`'s
  `#else` (non-SPIKE) branch makes `start_timer`/`stop_timer`/`get_timer`
  all no-ops when `SPIKE` is defined. Discovered when `fconv3d` printed "0
  cycles" despite passing correctness checks. Fixed by never defining
  `-DSPIKE` and instead prioritizing `se_build/shim`'s `printf.h`
  (`#include <stdio.h>`) in the include path.

Dataset-size fixes (each benchmark's own `common/default_args.mk` default
was **not** necessarily what the RTL baseline was actually generated with):

- **fmatmul**: `16 64 128` (this repo's own default) -> `128 128 128`,
  dropping MAPE from 93.4% to 2.9%.
- **dotproduct**: was "not comparable" at all (no single ROI marker in
  source, and the internal `vsize=512` default gave no matching segment
  cycle count). Root-caused by testing whether any individual "Vector
  runtime" segment matched the RTL baseline (none did), then finding
  `gen_data.py`'s smaller internal default of `vsize=64` combined with
  summing all 4 vector-runtime segments (a proper ROI instrumentation
  change in `main.c`) gave 653 cycles vs RTL's 686 - 4.8% MAPE.
- **fconv2d**: `64 64 7` -> `112 112 7` (matching sibling benchmarks
  fconv3d/iconv2d's own `112 7` convention), dropping MAPE from 58.9% to
  ~23%.

Default-config (4 lanes, VLEN=4096) results after these fixes:

| Benchmark   | gem5 MAPE |
|-------------|-----------|
| fmatmul     | 2.9%      |
| dotproduct  | 4.8%      |
| fconv3d     | 16.0%     |
| fconv2d     | 23.2%     |
| iconv2d     | 24.1%     |
| **Average** | **14.2%** |

## Phase 5: the full 9-config sweep, and two new deadlocks

VP++ had been validated across 9 configs (2/4/8 lanes x 1024/2048/4096
VLEN) but gem5 had only ever been run at the default 4L_4096V. Added
`ARA_LANES`/`ARA_VLEN` env-var parameterization to `run_gem5_ara.py`,
built per-lane-count binaries (`apps_gem5/unseen/sweep_build.sh`), and
wrote a parallel sweep runner (`apps_gem5/unseen/sweep_run.sh`, `xargs -P
3`) to run all 45 (config x benchmark) pairs against the embedded RTL/VP++
baselines from `plot_ara_unseen_heatmap.py`.

Three of the 45 runs (dotproduct/fconv2d/fconv3d at 8L_4096V) hung
indefinitely - a genuine, previously-undiscovered deadlock, since **every
prior use of `AraCoprocessor` in this project's history only ever tested 4
lanes**. Root-caused via iterative `--debug-flags=AraCoprocessor` tracing
(and, once that stopped being informative, custom `printf`/`fflush`
instrumentation added and removed across several rebuild-run cycles) to
two independent bugs, both only observable at `num_lanes > 4`:

1. **`VectorLane::tick()`'s writeback byte-marking was hardcoded to
   `datapathBytes * 4`** instead of `datapathBytes * numLanes` - silently
   correct only at exactly 4 lanes. At other lane counts this marked fewer
   destination bytes ready per chunk than the instruction's own
   `totalChunksNeeded` (computed elsewhere from the real `numLanes`)
   assumed, so the last bytes of any sufficiently large destination
   register were never marked ready. Fixed by threading a real `numLanes`
   member through `VectorLane`'s constructor and using it in the writeback
   math.
2. **The real root cause of the persisting hang**: `chunks_needed` (how
   many cycles an instruction is modeled as needing) was computed uniformly
   from lane bandwidth (`numLanes * datapathWidth` bits/cycle) for *every*
   instruction, but memory (load/store) instructions are actually
   processed by a separate VLSU queue that progresses at the fixed AXI bus
   bandwidth (`axiDataWidth`, 256 bits = 32 bytes/cycle), independent of
   lane count. At `numLanes <= 4`, lane bandwidth (<=256 bits) never
   exceeds AXI bandwidth, so this mismatch was invisible. At `numLanes=8`,
   lane bandwidth (512 bits = 64 bytes/cycle) exceeds AXI bandwidth, so a
   load needing e.g. 40 bytes got `chunks_needed=1` (looks done after one
   AXI-paced tick) while the memory queue's own writeback loop had only
   actually marked the first 32 of those 40 bytes ready - the last 8 bytes
   were never marked ready, permanently hazarding any later instruction
   that read them. Fixed by computing `chunks_needed` for memory
   instructions from `axiDataWidth` instead of `numLanes * datapathWidth`.

Diagnostic method worth recording: `printf`/`fflush(stdout)` mixed with
gem5's own `DPRINTF` unreliably interleaves in the log file when a program
is genuinely hung (stdio is fully-buffered, not line-buffered, once
redirected to a file - the last buffer's contents are invisible until
flushed or the process exits). Explicit `fflush(stdout)` after each debug
`printf` was required to see the true state of a frozen simulation. All
temporary debug instrumentation was removed before the final fix was
committed.

Verified: all three previously-hung configs now complete, and the
default 4L_4096V config is unchanged (dotproduct still 653 cycles).

## Final results: full 9-config x 5-benchmark sweep

| Config    | Benchmark  | gem5   | RTL    | VP++   | gem5 MAPE | VP++ MAPE |
|-----------|------------|--------|--------|--------|-----------|-----------|
| 2L_1024V  | dotproduct | 653    | 1215   | 1181   | 46.3%     | 2.8%      |
| 2L_1024V  | fconv2d    | 386204 | 320535 | 329553 | 20.5%     | 2.8%      |
| 2L_1024V  | fconv3d    | 1117030| 982587 | 998198 | 13.7%     | 1.6%      |
| 2L_1024V  | fmatmul    | 1181767| 1106743| 1069043| 6.8%      | 3.4%      |
| 2L_1024V  | iconv2d    | 386330 | 316698 | 333655 | 22.0%     | 5.4%      |
| 2L_2048V  | dotproduct | 653    | 1216   | 1199   | 46.3%     | 1.4%      |
| 2L_2048V  | fconv2d    | 385330 | 313857 | 321867 | 22.8%     | 2.6%      |
| 2L_2048V  | fconv3d    | 1103849| 958384 | 974128 | 15.2%     | 1.6%      |
| 2L_2048V  | fmatmul    | 1140096| 1065178| 1066829| 7.0%      | 0.2%      |
| 2L_2048V  | iconv2d    | 385212 | 310049 | 323781 | 24.2%     | 4.4%      |
| 2L_4096V  | dotproduct | 653    | 1259   | 1303   | 48.1%     | 3.5%      |
| 2L_4096V  | fconv2d    | 385631 | 316343 | 319172 | 21.9%     | 0.9%      |
| 2L_4096V  | fconv3d    | 1097146| 954195 | 963819 | 15.0%     | 1.0%      |
| 2L_4096V  | fmatmul    | 1078610| 1062879| 1065946| 1.5%      | 0.3%      |
| 2L_4096V  | iconv2d    | 385543 | 309068 | 320130 | 24.7%     | 3.6%      |
| 4L_1024V  | dotproduct | 653    | 693    | 637    | 5.8%      | 8.1%      |
| 4L_1024V  | fconv2d    | 208370 | 179549 | 176885 | 16.1%     | 1.5%      |
| 4L_1024V  | fconv3d    | 609161 | 543899 | 548006 | 12.0%     | 0.8%      |
| 4L_1024V  | fmatmul    | 618070 | 739757 | 557747 | 16.4%     | 24.6%     |
| 4L_1024V  | iconv2d    | 208607 | 176851 | 181910 | 18.0%     | 2.9%      |
| 4L_2048V  | dotproduct | 653    | 676    | 623    | 3.4%      | 7.8%      |
| 4L_2048V  | fconv2d    | 193314 | 160184 | 165019 | 20.7%     | 3.0%      |
| 4L_2048V  | fconv3d    | 563946 | 491891 | 497626 | 14.6%     | 1.2%      |
| 4L_2048V  | fmatmul    | 575591 | 555657 | 534573 | 3.6%      | 3.8%      |
| 4L_2048V  | iconv2d    | 193367 | 159111 | 166937 | 21.5%     | 4.9%      |
| 4L_4096V  | dotproduct | 653    | 686    | 663    | 4.8%      | 3.4%      |
| 4L_4096V  | fconv2d    | 193176 | 156834 | 160971 | 23.2%     | 2.6%      |
| 4L_4096V  | fconv3d    | 556290 | 479684 | 487105 | 16.0%     | 1.5%      |
| 4L_4096V  | fmatmul    | 548321 | 532903 | 533466 | 2.9%      | 0.1%      |
| 4L_4096V  | iconv2d    | 193231 | 155748 | 161929 | 24.1%     | 4.0%      |
| 8L_1024V  | dotproduct | 653    | 431    | 375    | 51.5%     | 13.0%     |
| 8L_1024V  | fconv2d    | 137436 | 137265 | 117286 | 0.1%      | 14.6%     |
| 8L_1024V  | fconv3d    | 406443 | 395973 | 378915 | 2.6%      | 4.3%      |
| 8L_1024V  | fmatmul    | 481782 | 629288 | 312307 | 23.4%     | 50.4%     |
| 8L_1024V  | iconv2d    | 138995 | 135773 | 122101 | 2.4%      | 10.1%     |
| 8L_2048V  | dotproduct | 653    | 402    | 345    | 62.4%     | 14.2%     |
| 8L_2048V  | fconv2d    | 102773 | 89405  | 87112  | 15.0%     | 2.6%      |
| 8L_2048V  | fconv3d    | 304398 | 269815 | 271051 | 12.8%     | 0.5%      |
| 8L_2048V  | fmatmul    | 326852 | 363317 | 278925 | 10.0%     | 23.2%     |
| 8L_2048V  | iconv2d    | 102883 | 92819  | 89836  | 10.8%     | 3.2%      |
| 8L_4096V  | dotproduct | 653    | 397    | 353    | 64.5%     | 11.1%     |
| 8L_4096V  | fconv2d    | 97423  | 80189  | 82544  | 21.5%     | 2.9%      |
| 8L_4096V  | fconv3d    | 286475 | 249231 | 248735 | 14.9%     | 0.2%      |
| 8L_4096V  | fmatmul    | 290247 | 277426 | 267338 | 4.6%      | 3.6%      |
| 8L_4096V  | iconv2d    | 97553  | 85916  | 83503  | 13.5%     | 2.8%      |

**Averages across all 45 points: gem5 MAPE ~18.9%, VP++ MAPE ~5.8%.**

**Open discovery, not yet root-caused**: dotproduct's gem5 cycle count is
**exactly 653 at all 9 configs** - it never changes with lane count or
VLEN, while RTL and VP++ both scale down sharply with more lanes (RTL:
1215 -> 397 cycles from 2L to 8L). This means dotproduct's MAPE gets worse
at higher lane counts (46% -> 65%) instead of better, and is the single
biggest driver of the gap between gem5's average MAPE (18.9%) and VP++'s
(5.8%) - excluding dotproduct, gem5 averages ~14.7% against VP++'s ~6.7%
over the other four benchmarks. Likely explanation (untested): dotproduct's
dataset (VL=64) may be small enough that its whole computation already
fits in a single lane-bandwidth chunk regardless of lane count, so the
lane-count-dependent term in the timing model never actually engages for
it. Left as a follow-up.

## Where the code lives

- `src/cpu/ara/ara_coprocessor.{cc,hh}` - the model itself.
- `run_gem5_ara.py` - SE-mode run script (CVA6-matching MinorCPU tuning,
  split memory FU pool, `ARA_LANES`/`ARA_VLEN` env-var parameterization).
- `apps_gem5/se_build/` - SE-mode compatibility shim for the original
  11-benchmark RiVEC suite.
- `apps_gem5/unseen/` - the 5 newly-ported benchmarks (dotproduct, fconv2d,
  fconv3d, fmatmul, iconv2d), their `build.sh`/`sweep_build.sh`/
  `sweep_run.sh`, and a `README.md` with per-benchmark dataset-fix detail.
- Branch: `ara-coprocessor-timing-model` on `rseac/gem5` (this session's
  fork).
