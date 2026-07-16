# Branch Structure

This branch adds two new features to the base gem5 Ara simulator: 
- Custom memory requests with additional fields for vector instructions (see `EXTENSIONS.md`)
- Configurable memory hierarchies differentiating between scalar and vector accesses (see `MEMORY_CONFIG.md`)

# Memory Hierarchy Configurations

## Isolated Vector Memory Hierarchy

This configuration is enabled with the `--scalar-uncacheable` flag. Disables caching for scalar accesses and redirects them to DRAM directly instead. See `MEMORY_CONFIG.md` for more details.

## Parallel Vector Memory Hierarchy

This configuration is enabled with the `--vector-cache` flag. Creates a separate `PrivateL1L2CacheHierarchy` object for vector accesses. See `MEMORY_CONFIG.md` for more details.

NOTE: The above configurations are meant to be used independently

## Selectable Vector Prefetcher

Pick and tune a hardware prefetcher (`none`/`stride`/`imp`/`vimp`/`isb`/`stems`) from the command line and attach it to one cache via `--prefetcher`, `--prefetcher-side {scalar,vector}`, `--prefetcher-level {l1,l2}`, and repeatable `--pf-param NAME=VALUE`. Selecting a prefetcher forces `--vector-cache`; every other cache is left prefetcher-free. See `MEMORY_CONFIG.md` for the mechanism and the per-prefetcher `--pf-param` reference. `vimp` is this fork's vector indirect memory prefetcher — see the IMP vs VIMP section below.

NOTE: prefetching is now **disabled on every cache by default** (the stdlib caches previously attached a `StridePrefetcher` everywhere) — see the behavior-change note in `MEMORY_CONFIG.md` before comparing against older runs.

## IMP Data-Read Fix (`IndirectMemoryPrefetcher`)

`IndirectMemoryPrefetcher` (IMP) is the **only** prefetcher in `src/mem/cache/prefetch/` that reads the *data* of a triggering access. It reads the value at the accessed address, treats it as an array index, and prefetches `baseAddr + (index << shift)` — the classic `A[B[i]]` gather pattern. The read happens in `IndirectMemory::calculatePrefetch` via `pfi.get<uintN_t>(byteOrder)` (`src/mem/cache/prefetch/indirect_memory.cc:112-126`).

### Original problem

On a **read hit**, IMP panicked:

```
panic: PrefetchInfo::get called with a request with no data.
```

from `PrefetchInfo::get` (`src/mem/cache/prefetch/base.hh:228`).

**What gem5 did.** When the cache notifies a prefetcher it builds a `PrefetchInfo` whose data buffer is only filled when `pkt->hasData()` is true (`PrefetchInfo` constructor, `src/mem/cache/prefetch/base.cc:66`). But `hasData()` is a **command-attribute** test — whether the packet's command *class* is *defined* as carrying data — not whether bytes are physically present (`Packet::hasData` → `cmd.hasData`, `src/mem/packet.hh:614`). A read is a `ReadReq` on the way in (`{IsRead, IsRequest, NeedsResponse}`, `src/mem/packet.cc:73`); only the `ReadResp` command has the `HasData` attribute (`src/mem/packet.cc:75`).

**Why a hit still had "no data".** IMP runs on the **Hit/Miss** probe path (`probeNotify`), where the packet is still a `ReadReq`. The cache fires the Hit probe (`src/mem/cache/base.cc:501`) *before* converting the request into a response (`handleTimingReqHit`, `src/mem/cache/base.cc:509`). Yet by that point `access()` has **already** copied the cache block's bytes into the request packet's own buffer (`setDataFromBlock`, called from `access()` at `src/mem/cache/base.cc:481`) in order to satisfy the read. So the index value IMP needs is physically sitting in the `ReadReq` packet — but `PrefetchInfo` discards it, purely because `hasData()` (a command-class flag) is false for a request. `pfi.get<>()` then dereferences the null `data` pointer and panics.

(The only packet with `HasData` set is a `ReadResp`, which appears on the **Fill** probe — but IMP does not override `notifyFill`, so it never read indices from there.)

### Fix

The `PrefetchInfo` constructor now also captures the buffer for a *satisfied demand-read hit*, since on a hit the bytes are already in the packet (`src/mem/cache/prefetch/base.cc:66`):

```cpp
bool read_hit_data = !miss && pkt->isRead() && !pkt->cmd.isHWPrefetch();
if ((!write && miss) || (!pkt->hasData() && !read_hit_data)) {
    data = nullptr;
} else { /* copy req_size bytes out of the packet buffer */ }
```

- `!miss` — the read was satisfied, so the block's bytes are in the packet.
- `pkt->isRead()` — writes already carry data and are handled by the existing `hasData()` branch.
- `!pkt->cmd.isHWPrefetch()` — a demand read has a CPU-allocated data buffer; a hardware-prefetch read does not, so reading from it would dereference an unallocated pointer.

This is one of two **core-file** changes on this branch (the other is the IPD idx2 bound below). It is low-risk because IMP is the only prefetcher that reads `PrefetchInfo.data` — every other prefetcher ignores it, so their behavior is unchanged.

## IMP IPD idx2 Miss-Tracking Bound (`IndirectMemoryPrefetcher`)

IMP's Indirect Pattern Detector (IPD) confirms a candidate `A[B[i]]` pattern in two phases: record candidate base addresses from the misses following the first index read (`idx1`), then compare the misses following the second index read (`idx2`) against those candidates.

### Original problem

**What gem5 did.** Phase 1 is bounded: `trackMissIndex1` counts misses and stops tracking after `addr_array_len` (= `baseAddr.size()`, default 4) of them (`src/mem/cache/prefetch/indirect_memory.cc:215-218`). Phase 2 was **not**: `trackMissIndex2` compared each miss against the recorded candidates and, on no-match, simply returned — no counter, no limit, `ipdEntryTrackingMisses` left armed. Since every miss is diverted into the tracking branch while that pointer is set (`indirect_memory.cc:80`), a phase-2 entry that never finds a match consumes **every subsequent miss forever**, starving the stream detector. The only other exits are a pattern match or a *third* index read (`allocateOrUpdateIPDEntry`) — and on the vector hierarchy, index reads are only observed on hits to prefetched lines (`prefetch_on_pf_hit`, see `src/mem/cache/prefetch/base.cc:181-186`), which stop arriving as soon as issuing stops. Deadlock: no prefetches → no observed index reads → no release → no prefetches. The garbage "indices" that arm this state are gather-element data values (see the data-read fix above), so RVV gather workloads hit it readily.

**What the paper says.** The design intends both windows to be short and symmetric (IMP paper, Sec. 3.2.2): "IPD only tracks the first few misses after the idx1 access" and "IPD pairs later cache misses with idx2 to compute BaseAddrs, **as it did with idx1**"; false patterns are limited by "only considering cache misses **soon after** the index access". The unbounded phase 2 was a gem5 implementation gap, masked in the paper's scalar setting where a third index read always arrives within a couple of loop iterations.

### Fix

`trackMissIndex2` now has the same miss budget as phase 1: a per-entry counter `numIdx2Misses` (`src/mem/cache/prefetch/indirect_memory.hh:141`) increments on every no-match comparison, and when it reaches `baseAddr.size()` the IPD entry is invalidated and miss tracking released (`src/mem/cache/prefetch/indirect_memory.cc:252-263`). A released index stream can re-allocate a fresh IPD entry on its next index read, matching the paper's "the index array can keep allocating IPD entries in the future". Detection episodes are therefore always bounded: at most `addr_array_len` misses per phase, after which the stream engine resumes.

**Known remaining deviation** (not fixed in IMP): gem5 routes each miss to *either* the IPD *or* the prefetch/stream table (the if/else at `indirect_memory.cc:80`), whereas in the paper (Fig. 3) the IPD is a side structure and the stream table sees every access. With the idx2 bound the hijack now lasts at most ~8 misses per episode instead of forever, but the structural serialization remains. VIMP initially inherited the same single-tracker structure and demonstrably failed on it (multiple streaming loads per loop body kept re-claiming the tracker; zero detections on `tsvc_vec`); it now uses per-entry concurrent tracking instead — see the IMP vs VIMP section.

### `IMP` debug flag (IPD lifecycle tracing)

Stock IMP has no `DPRINTF`s at all — every conclusion about IPD behavior previously had to be inferred from `HWPrefetch` candidate-trace signatures. A dedicated `IMP` debug flag (`src/mem/cache/SConscript:62`) now traces every IPD entry lifecycle transition in `indirect_memory.cc`:

| event | message |
|---|---|
| entry allocated (first index read) | `IPD: PT <id> allocated: idx1=...` |
| second index read | `IPD: PT <id> armed: idx1=... idx2=..., tracking misses` |
| idx1 miss window full (4 misses) | `IPD: PT <id> idx1 window full ..., tracking paused until idx2` |
| pattern match → promoted to PT | `IPD: PT <id> pattern DETECTED: baseAddr=... shift=...` |
| **dropped**: third index read, no match | `IPD: PT <id> DROPPED: third index read ...` |
| **dropped**: idx2 miss budget exhausted | `IPD: PT <id> DROPPED: idx2 miss budget exhausted ...` |
| **dropped**: evicted by replacement | `IPD: PT <id> DROPPED: evicted for PT <id2> ...` |

`PT <id>` is the prefetch-table-entry identity the IPD entry is keyed by, stable across one entry's lifetime. Usage: `--debug-flags=IMP` (combine with `HWPrefetch` to correlate detector state with issued candidates). Output volume is tiny compared to `HWPrefetch` — one line per detector transition, none per prefetch.

# IMP vs VIMP: Element-wise vs Chunk-wise Indirect Prefetching

This section compares the stock **Indirect Memory Prefetcher** (`imp`,
`IndirectMemoryPrefetcher`, `src/mem/cache/prefetch/indirect_memory.{hh,cc}`,
from Yu et al., *IMP: Indirect Memory Prefetcher*, MICRO 2015) with this
fork's **Vector Indirect Memory Prefetcher** (`vimp`,
`VectorIndirectMemoryPrefetcher`,
`src/mem/cache/prefetch/vector_indirect_memory.{hh,cc}`).

VIMP is a generalization of IMP, not a replacement: it keeps IMP's tables,
learning scheme, and confidence machinery, and changes the *training unit*
from one array element to one vector register's worth of elements. Deeper
mechanism documentation lives in DOCUMENTATION.MD ("Vector Indirect Memory
Prefetcher"); invocation and tunables live in MEMORY_CONFIG.md.

## The pattern both prefetchers target

Both attack the indirect pattern `A[B[i]]`: a sequential walk of an index
array `B` whose values address a target array `A`. Both split the problem
into the same two halves:

1. a **stream prefetcher** that keeps the (perfectly sequential) index
   array `B` flowing into the cache, and
2. an **indirect prefetcher** that reads the index *values* and computes
   the scattered targets `A[B[i]] = baseAddr + (B[i] << shift)`.

The difference is what the walk of `B` looks like at the cache. Scalar code
presents it one element per access; vectorized RVV code presents it one
vector register per access — a single unit-stride load covering
`VLEN·LMUL/8` bytes each loop iteration:

```
scalar loop:   B[0], B[1], B[2], B[3], ...            one 4-8 B access each
vector loop:   B[0:15], B[16:31], B[32:47], ...       one 64 B access each
                                                       (VLEN=512, int32 indices)
```

**IMP monitors the element stream; VIMP monitors the chunk stream.** A VIMP
"stream" is a PC whose successive accesses are back-to-back VLEN-wide
chunks across loop iterations — each new address is exactly the previous
address plus the previous request's size — and every observed chunk carries
`VLEN/EEW` index values at once (16 for VLEN=512 with `int32` indices)
instead of one. Correspondingly, one VIMP trigger prefetches a whole
chunk's worth of `B` (and a whole chunk's worth of `A`-targets) instead of
advancing element by element.

## What they share

- **Queued-prefetcher skeleton.** Both subclass `prefetch::Queued`
  (`src/mem/cache/prefetch/queued.hh`), so candidate block-alignment,
  queue dedup/squash, page handling, translation, and throttling are
  identical infrastructure.
- **Two-table design.** A PC-indexed **Prefetch Table** (PT) holding
  stream state plus the learned `(baseAddr, shift, confidence)` indirect
  state, and a small **Indirect Pattern Detector** (IPD) used only during
  learning. Same default geometry in both: PT 16 entries / 16-way LRU,
  IPD 4 entries / 4-way LRU. The IPD is keyed by the PT entry's pointer in
  both (`indirect_memory.cc:178`, `allocateOrUpdateIPDEntry` in
  `vector_indirect_memory.cc`).
- **Index–miss correlation with two-value confirmation.** Both learn
  `(baseAddr, shift)` by relating observed index *values* to miss
  addresses under a candidate shift list, and both require agreement from
  two *distinct* index values before enabling (`trackMissIndex1/2` in
  IMP, `trackMiss` in VIMP), bounded by the shared `addr_array_len`
  parameter. The *structure* differs sharply — IMP's read-clocked
  two-phase single tracker vs VIMP's alignment-free per-entry pair
  matching — see the table below.
- **Confidence gating.** A per-entry saturating counter
  (`num_indirect_counter_bits`, default 3) incremented when a demand
  access matches a predicted target (`checkAccessMatchOnActiveEntries`,
  both files) and decremented after a training step with no match;
  indirect prefetches are only issued while
  `indirectCounter > prefetch_threshold` (default 2). A wrong
  `(baseAddr, shift)` therefore self-disables instead of spraying.
- **Target arithmetic.** `pf_addr = baseAddr + (index << shift)` with a
  configurable shift candidate list, letting the learned shift absorb the
  element-size scaling the code performs (e.g. TSVC's `vsll` before
  `vluxei64`).
- **Payload dependence.** Both can only read index values on accesses that
  *hit* the attached cache, relying on the `PrefetchInfo` read-hit data
  capture documented in the IMP Data-Read Fix section above.
- **Shared training thresholds.** `stream_counter_threshold(4)`,
  `addr_array_len(4)`, `prefetch_threshold(2)`,
  `num_indirect_counter_bits(3)` have the same defaults in both classes,
  deliberately, so cross-prefetcher comparisons vary one mechanism at a
  time.

## Mechanism-by-mechanism differences

| Mechanism | IMP (`indirect_memory.cc`) | VIMP (`vector_indirect_memory.cc`) |
|---|---|---|
| Training unit | One array element: an access qualifies as an index read only if `getSize() <= 8` (`:114`) | One chunk = one access of any size; payload sliced into `index_size`-byte elements (`extractIndices`), up to `max_indices_per_chunk` per event |
| Stream detection | *Any* address change counts (`pt_entry->address != addr`, `:96`); the counter never resets, and the stream prefetch reuses whatever delta was last observed (`:100-103`) — random-access PCs eventually "stream" along garbage deltas | Strict contiguity: `addr == prev_addr + prev_size`; a discontinuity resets the counter (but keeps the learned pattern). Chunk width comes from the request, so any VLEN/LMUL/EEW works with no VLEN parameter, and random-access PCs never stream |
| Index visibility on RVV code | None: the vlenb-sized unit-stride index load fails the ≤ 8 B gate, so the indirect half is dead on vectorized loops (verified on TSVC) | The wide load *is* the training event; one notification delivers all `VLEN/EEW` indices via `PrefetchInfo::get<T>(endian, index)` (`src/mem/cache/prefetch/base.hh`) |
| Who may train the IPD | Any not-yet-enabled PT entry whose address changed and whose payload is readable — including pointer-chasing or gather PCs | Only entries whose *chunk stream is already confirmed*, keeping gather-element PCs out of the detector |
| IPD correlation input | Single `idx1`/`idx2` values recorded at two consecutive index reads; base candidates dimensioned `[miss][shift]`; assumes the misses in between belong to those reads' iterations | Leading `ipd_indices_per_chunk` (8) indices of each of the last `ipd_chunk_history` (16) chunks, plus the last `addr_array_len` (4) tracked miss addresses; a pattern is confirmed in one step when a miss-pair delta equals a *distinct*-index-pair shifted delta within one remembered chunk — no phases, no read/miss alignment assumption |
| Which misses correlate | Every miss of any size while tracking — including wide line fills of `B` itself, which pollute the candidate set | Only element-sized (≤ 8 B) accesses; wide accesses take the table path instead, so the stream keeps training and the second chunk can still arm the detector |
| Miss-tracking structure | One global `ipdEntryTrackingMisses` slot: whichever stream trained last owns *all* subsequent misses. With several streaming loads per loop body — and out-of-order issue placing the independent ones between the index load and its dependency-stalled gather — the gather's misses usually train the wrong entry (measured on `tsvc_vec`: 5012 training episodes, 0 detections under this scheme) | Per-entry concurrent tracking: every valid IPD entry observes every element-sized miss independently (the IMP paper's side-structure model). Combined with the chunk history, this also tolerates the index reads racing up to `ipd_chunk_history` chunks ahead of their gathers' MSHR-throttled misses — phase-aligned schemes scored 0 detections on `tsvc_vec` even with per-entry tracking (1737 fully-compared episodes) |
| Enabled-state recovery | `enabled` is sticky: a falsely learned pattern silently blocks the stream from retraining until its PT entry is evicted | Demotion: an enabled entry at zero confidence for `demotion_chunks` (16) consecutive chunks is disabled and retrains (`patternsDemoted` stat) |
| Targets per trigger | One line: the `delta` loop (`:151-158`) re-pushes `baseAddr + (index << shift)` — the same address `distance` times — which the queue dedups to a single line. Effectively zero lookahead in the gem5 implementation | One target per index in the chunk — the whole iteration's gather footprint — deduplicated at line granularity and capped by `max_indirect_targets` |
| Lookahead | None in gem5. (The IMP *paper* reads `B[i+Δ]` from the cache to prefetch ahead; gem5's version never implemented it) | `indirect_delta=0`: intra-chunk only (targets issue at index-load time while the gather drains element-serially). `indirect_delta=N`: targets for chunks up to N ahead, from index lines captured at cache-fill time (`notifyFill`) and released demand-clocked; effective lookahead `min(N, streaming_distance)` |
| Stream prefetch shape | `addr + delta·i` for i ∈ [1, `streaming_distance`], delta = last observed address change | Every cache line covered by the next `streaming_distance` *chunks* (multi-line for VLEN·LMUL/8 > 64 B) |
| Shift handling | Default `shift_values = [2, 3, 4, -3]`; negative shifts computed as `idx << shift` — undefined behavior for negatives | Default `[0, 1, 2, 3, 4]`; `applyShift` maps negative shifts to an arithmetic right shift |
| Confidence cadence | Counter check/decrement once per *index read*; match test against the single current index | Once per *chunk*; match test against all indices of the current chunk |
| Address space (as wired in `rvv/`) | Physical by default: with no MMU registered, every page-crossing target is dropped (`Queued::insert`, `src/mem/cache/prefetch/queued.cc:220`) — on TSVC s4112 that was 100% of the scalar-detected targets | `use_virtual_addresses = True` class default; the config layer auto-registers the CPU MMU (`prefetcher_factory.needs_mmu()` → `registerMMU(cpu.core.mmu)` in `rvv/vector_cache_hierarchy.py`), so cross-page targets are translated and issued. (`imp` can opt in with `--pf-param use_virtual_addresses=true`, which also registers the MMU) |
| Observation defaults | `prefetch_on_access = False` (hits observed only on prefetched lines), `queue_size = 32` | `prefetch_on_access = True` (index payloads are only readable on hits, and steady-state index loads hit on lines VIMP itself streamed in), `queue_size = 64`, `on_inst = False` |
| Extra structures | — | Expected-fill table (`pending_fill_entries`) and per-entry captured-index buffer (`pending_index_sets`), both delta-mode only |
| Stats / debug | Base/Queued stats; `--debug-flags=IMP` | Adds `vimpStats.{chunksSliced, indicesExtracted, streamCandidates, indirectCandidates, patternsDetected, ipdDropsNoMatch, fillLinesCaptured, pendingSetsDropped}`; `--debug-flags=VIMP` |

## Behavior on the same code

**Vectorized indirect loop** (`vle32` indices → `vluxei64` gather): IMP
never reads an index (size gate), its IPD trains on whatever payloads do
qualify — e.g. gather elements whose "indices" are float bits — and any
detected pattern's scattered targets are page-dropped without an MMU. Its
stream half still fires (delta-agnostic), which is where its measured
speedups on TSVC came from. VIMP trains on the index loads directly and
issues the batch targets; this is the workload it exists for.

**Scalar indirect loop** (`lw` index → scalar load of `A[idx]`): IMP's
designed case. VIMP's *stream half* degenerates gracefully — a 4 B access
with `index_size=4` is a one-element chunk, and `addr == prev + prev_size`
is exactly the element-stride check — but its *pattern detector* does
not: the pair invariant needs two distinct indices within one chunk, and
scalar chunks hold one. For scalar baselines use `imp`, the unmodified
reference design. (Restoring scalar detection would mean allowing pairs
across single-element history sets — the invariant holds across
iterations too — at a higher false-match cost; not currently
implemented.)

## Parameter correspondence

| IMP | VIMP | Notes |
|---|---|---|
| `pt_table_*`, `ipd_table_*` | same names | identical geometry/defaults |
| `shift_values [2,3,4,-3]` | `shift_values [0,1,2,3,4]` | VectorParam — not settable via `--pf-param` |
| `addr_array_len(4)` | `addr_array_len(4)` | IMP: misses recorded per idx1 window + idx2 comparison budget; VIMP: recent-miss FIFO depth for pair matching |
| — | `ipd_chunk_history(16)` | chunk index-set history per IPD entry (OoO lag tolerance) |
| — | `demotion_chunks(16)` | zero-confidence chunks before an enabled entry retrains (0 = sticky like IMP) |
| `prefetch_threshold(2)` | `prefetch_threshold(2)` | same meaning |
| `stream_counter_threshold(4)` | `stream_counter_threshold(4)` | counts changed accesses (IMP) vs contiguous chunks (VIMP) |
| `streaming_distance(4)` | `streaming_distance(4)` | delta-multiples (IMP) vs chunks, all covered lines (VIMP) |
| `num_indirect_counter_bits(3)` | `num_indirect_counter_bits(3)` | same meaning |
| `max_prefetch_distance(16)` | — | scales IMP's issue-loop iteration count, which re-pushes one address; no VIMP equivalent |
| — | `index_size(4)`, `index_signed(True)` | payload slicing: the index load's EEW/8 |
| — | `max_indices_per_chunk(64)` | slice cap per chunk |
| — | `ipd_indices_per_chunk(8)` | leading indices used for correlation |
| — | `max_indirect_targets(32)` | per-chunk target cap (post line-dedup) |
| — | `ipd_train_on_hits(False)` | correlate hits too (warm-cache experiments) |
| — | `indirect_delta(0)` | 0 = current-chunk targets; N = fill-capture lookahead |
| — | `pending_fill_entries(32)`, `pending_index_sets(8)` | delta-mode structures |

## Choosing a prefetcher in experiments

- `--prefetcher imp` — reference indirect prefetcher; meaningful on
  *scalar* (novec) binaries; on vector binaries only its stream half
  engages, which makes it the "what does the state of the art do here"
  baseline.
- `--prefetcher vimp` — the vector-aware design; on scalar binaries it
  approximates IMP (degenerate chunks), on vector binaries it is the
  design under test. Start with `--prefetcher-level l1` (see the warm-L2
  training caveat in MEMORY_CONFIG.md) and sweep `indirect_delta`
  together with `streaming_distance`.

Both are selected and tuned identically through
`rvv/riscv-rvv-se-ara-prefetcher.py` (`--prefetcher`, `--prefetcher-side`,
`--prefetcher-level`, `--pf-param NAME=VALUE`).

# Building

```
make build PROG_DIR=<program_dir> PROG=<progam_executable> # default gem5 configuration
make build PROG_DIR=<program_dir> PROG=<progam_executable> UC=1 # isolated vector memory hierarchy
make build PROG_DIR=<program_dir> PROG=<progam_executable> VC=1 # parallel vector memory hierarchy
```

- Running tests with UC=1 or VC=1 will produce output products in two output directories: `m5out` for base gem5 config and `m5out2` for the isolated or parallel vector memory hierarchy config
- This was done to verify program outputs between base and modified configurations

# Organizing TSVC Results (`tsvc-gem5/organize_results.py`)

Files a gem5 TSVC run's output into a structured results tree, splitting a multi-kernel
run's single `stats.txt` into one directory per kernel:

```
tsvc_results/<size>/<kernel>/<prefetcher>/iter_<N>/
    stats.txt        # just this kernel's ROI section, split out of the run
    config.ini, config.json, citations.bib   # the run's config (copied)
    run_info.txt     # size, kernel, prefetcher, iterations, LEN_1D/2D, source dir
```

## Usage

You run gem5 to any `-d` output dir, then point the script at it with a size label:

```bash
# one organize call fans a whitelist run out into every kernel's directory
python3 tsvc-gem5/organize_results.py --size tiny  out/base
python3 tsvc-gem5/organize_results.py --size tiny  out/stride
python3 tsvc-gem5/organize_results.py --size medium out/imp
```

## What you provide vs. what is auto-detected

| | Source |
|---|---|
| `--size {tiny,small,medium}` | you (the label) |
| `iterations` → `iter_<N>` (and `LEN_1D`/`LEN_2D`, recorded in `run_info.txt`) | `tsvc-gem5/src/common.h` (first uncommented `#define`) |
| prefetcher (`base`/`stride`/`imp`/`isb`/`stems`) | `<outdir>/config.ini` (attached prefetcher type; all `Null` → `base`) |
| which kernels ran, in order | whitelist embedded in the binary ∩ `RUN_KERNEL` order in `tsvc.c` |
| per-kernel `stats.txt` | `<outdir>/stats.txt` split on the ROI dump sections (trailing cumulative dump ignored) |

Because the array size and iteration count are compile-time (`common.h`) and the prefetcher
and kernel set are recorded by gem5, a single run of the 8-kernel whitelist binary is filed
into all 8 kernel directories automatically.

## Flags and edge cases

- `--kernel <name>` — required for a **single-kernel** `KERNELS="s353"` build (a lone whitelist
  name can't be recovered from the binary, since every kernel name also appears as a `strcmp`
  literal). Multi-kernel whitelists and runtime `--parms "s353 s491"` are auto-detected.
- `--include-dot` — also copy the graphviz `config.dot/.pdf/.svg` diagrams (large and identical
  per kernel; excluded by default). `--move` — delete the source dir after. `--dry-run` —
  print actions only. `--results-root <dir>` — write to an alternate tree.
- A **crashed run** (no completed ROI sections, e.g. the IMP gather panic) is refused with a
  clear message rather than writing partial junk.

## Workflow caveat

`iterations`/`LEN` are read from `common.h` **at organize time**, so organize each run
**before** editing `common.h` and rebuilding for the next size/iteration point. Order every
sweep point as: edit `common.h` → rebuild → run gem5 (all prefetchers) → organize → next point.