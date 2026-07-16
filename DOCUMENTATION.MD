# Fork Change Documentation

Each entry explains the method stock gem5 used, what this fork changed, and
why — with citations into the codebase.

## Fix: false IllegalInstFault on segment loads/stores under fractional LMUL

**Files:** `src/arch/riscv/isa/templates/vector_mem.isa` (four sites: the
`execute`/`initiateAcc` bodies of `VlSegMicroExecute`, `VlSegMicroInitiateAcc`,
`VsSegMicroExecute`, `VsSegMicroInitiateAcc`)

**Spec:** all ISA citations below refer to *RISC-V "V" Vector Extension,
Version 1.0* (ratified), <https://github.com/riscv/riscv-v-spec/releases/tag/v1.0>:

- §3.4.2 "Vector Register Grouping (`vlmul[2:0]`)" — LMUL meaning and
  encoding, including the fractional values (mf8/mf4/mf2 = `101`/`110`/`111`).
- §7.3 "Vector Load/Store Width Encoding" — vector memory instructions carry
  their own EEW in the encoding; `EMUL = (EEW/SEW) × LMUL`; encodings with
  EMUL outside [1/8, 8] are reserved.
- §7.8 "Vector Load/Store Segment Instructions" — segment ops require
  `EMUL × NFIELDS ≤ 8`, otherwise the encoding is reserved; each field
  occupies its own successively numbered register group.

### What stock gem5 did

Every segment load/store micro-op guarded legality with:

```cpp
const int64_t vlmul = vtype_vlmul(machInst.vtype8);
if (((1 << vlmul) * this->numFields) > 8)
    return std::make_shared<IllegalInstFault>("LMUL value is illegal ...");
```

This is wrong in two independent ways:

1. **Wrong quantity.** The spec constrains segment ops by
   `EMUL × NFIELDS ≤ 8` (V-spec 1.0 §7.8), where `EMUL = (EEW/SEW) × LMUL`
   (V-spec 1.0 §7.3). EEW comes from the instruction encoding (e.g. the `e32`
   in `vsseg2e32.v`, per §7.3), while the check only looked at the vtype LMUL
   (§3.4.2). The two diverge whenever EEW ≠ SEW — which GCC produces
   routinely, because its vsetvl pass canonicalizes vtype to an e8-based
   encoding when a loop contains only EEW-encoded memory ops.
2. **Undefined behavior on fractional LMUL.** LMUL may be fractional
   (mf8/mf4/mf2, V-spec 1.0 §3.4.2), and `vtype_vlmul()`
   (`src/arch/riscv/utility.hh:270`) sign-extends accordingly, so mf4 returns
   −2 and the check computed `1 << -2` — UB that evaluates to a huge value on
   x86, making every fractional-LMUL segment op fault as illegal.

Observed failure: TSVC `s4117_modified` compiles to
`vsetvli e8,mf4` + `vsseg2e32.v` (EMUL = (32/8)·¼ = 1, footprint
2 registers — legal, accepted by spike/hardware), but gem5 panicked at commit
with `Illegal instruction ... LMUL value is illegal for vsseg inst`
(`src/arch/riscv/faults.cc`, SE mode panics on unhandled faults).

### What this fork does instead

The check now computes the effective EMUL in log2 form and tests the actual
spec conditions:

```cpp
const int64_t emul_log2 = findLsbSet(width_EEW(machInst.width))
    - findLsbSet(vtype_SEW(machInst.vtype8))
    + vtype_vlmul(machInst.vtype8);
if (emul_log2 < -3 || emul_log2 > 3 ||
    ((int64_t)this->numFields << std::max<int64_t>(emul_log2, 0)) > 8)
    return std::make_shared<IllegalInstFault>("EMUL * NFIELDS is illegal ...");
```

- `emul_log2 ∈ [−3, 3]` enforces EMUL ∈ [1/8, 8] (V-spec 1.0 §7.3: encodings
  outside this range are reserved).
- The footprint term uses `max(emul_log2, 0)` because a field with fractional
  EMUL still occupies one whole architectural register (V-spec 1.0 §7.8:
  fields live in successively numbered register groups).
- Helpers: `findLsbSet` (`src/base/bitfield.hh:369`, exact log2 for the
  power-of-two EEW/SEW values), `width_EEW`/`vtype_SEW`/`vtype_vlmul`
  (`src/arch/riscv/utility.hh`).

No change was needed in the macro-op constructors (`VlSegConstructor` /
`VsSegConstructor`): they already split micro-ops from `vl` and
`micro_vlmax = vlen / EEW` — i.e. in EMUL-register units — so execution is
correct once the false fault is removed.

### Behavior changes

| vtype + instruction | EMUL × NF | V-spec 1.0 §7.3/§7.8 | Old gem5 | New gem5 |
|---|---|---|---|---|
| `e8,mf4` + `vsseg2e32` | 1 × 2 = 2 | legal | faults (UB shift) | passes |
| `e64,m8` + `vsseg2e8` | 1 × 2 = 2 | legal | faults | passes |
| `e32,m4` + `vlseg2e32` | 4 × 2 = 8 | legal | passes | passes |
| `e8,m2` + `vsseg2e64` | 16 × 2 | illegal (EMUL > 8) | passes silently | faults |
| `e8,mf2` + `vsseg8e64` | 4 × 8 = 32 | illegal | passes silently | faults |

## Feature: Vector Indirect Memory Prefetcher (VIMP)

**Files:**

- `src/mem/cache/prefetch/vector_indirect_memory.hh` / `.cc` (new — the
  prefetcher)
- `src/mem/cache/prefetch/Prefetcher.py` (new `VectorIndirectMemoryPrefetcher`
  class, additive)
- `src/mem/cache/prefetch/SConscript` (register SimObject + source, additive)
- `src/mem/cache/SConscript` (new `VIMP` debug flag, additive)
- `src/mem/cache/prefetch/base.hh` (two additive `PrefetchInfo` accessors:
  `hasData()` and an element-indexed `get<T>(endian, index)`)
- `rvv/prefetcher_factory.py` (`vimp` entry + `needs_mmu()` helper)
- `rvv/vector_cache_hierarchy.py` (`prefetcher_needs_mmu` kwarg →
  `registerMMU(cpu.core.mmu)` on the attached prefetcher)
- `rvv/riscv-rvv-se-ara-prefetcher.py` (`--prefetcher vimp` choice + MMU
  plumbing)

### What stock gem5 did

The stock Indirect Memory Prefetcher
(`src/mem/cache/prefetch/indirect_memory.cc`, from Yu et al., MICRO 2015)
assumes the index array `B` of an `A[B[i]]` pattern is walked by *scalar*
loads, one element per access:

1. **Index reads must be ≤ 8 bytes.** The index value is read from the
   access's payload only when `pfi.getSize() <= 8`
   (`indirect_memory.cc:114`), with a `switch` on the size reading exactly
   one integer. A vectorized index walk issues one unit-stride load of
   `VLEN·LMUL/8` bytes per iteration (64 B at VLEN=512), so IMP never
   observes a single index value on RVV code and its indirect half is dead —
   this was verified experimentally on TSVC gathers and is not fixable with
   parameters.
2. **One index, one prefetch.** Even when trained, the steady-state code
   generates `baseAddr + (index << shift)` from the single current index
   (`indirect_memory.cc:151-158`; the `delta` loop re-pushes the same
   address, which the queue deduplicates), giving one target per index read
   and essentially no lookahead.
3. **Stream detection is delta-agnostic.** `pt_entry->address != addr`
   counts *any* address change as a streaming access and prefetches with the
   last observed delta (`indirect_memory.cc:96-105`). This half incidentally
   works at any granularity — the vector blocker is only (1) and (2) — but it
   also treats random-access PCs as "streams" and lets them allocate IPD
   entries.
4. **Page-crossing prefetches need an MMU.** `Queued::insert` only processes
   a candidate on another page if an MMU was registered
   (`can_cross_page = (mmu != nullptr)`, `src/mem/cache/prefetch/queued.cc`);
   otherwise it is dropped (`pfSpanPage` stat). No config in this repo
   registered one, and with physical-address training a scattered
   `base + (idx << shift)` target is only meaningful inside the trigger's own
   page frame anyway — measured on TSVC `s4112`, 100% of indirect candidates
   were page-dropped. The plumbing to fix this exists upstream but was
   unused: `BasePrefetcher.registerMMU()`
   (`src/mem/cache/prefetch/Prefetcher.py`), `Base::addMMU`
   (`src/mem/cache/prefetch/base.cc`), translation via
   `Queued::translationComplete` and `system->threads[contextId]`
   (`queued.cc`).
5. **`PrefetchInfo` captures the payload but exposes one value.** This
   fork's earlier fix (`base.cc`, `read_hit_data`) already copies the full
   request payload into `PrefetchInfo::data` for read *hits* (a read miss has
   no data yet). But the only accessor, `get<T>(endian)`, returns element 0
   (`base.hh`), so a wide payload's remaining elements were unreachable.

### What this fork does instead

`VectorIndirectMemory` (CLI name `vimp`) is a new `Queued` subclass in new
files — the stock IMP is left untouched as a baseline. A full side-by-side
comparison with stock IMP (shared machinery, per-mechanism differences,
parameter correspondence) is in PREFETCH.md, "IMP vs VIMP". It redefines
IMP's training unit from "element" to "chunk" (one wide unit-stride
access):

1. **Chunk-granular stream detection.** The PC-indexed prefetch table stores
   the last address *and last request size*; an access continues the stream
   iff `addr == prev_addr + prev_size`. The chunk size is whatever the
   request says, so any VLEN/LMUL/EEW (and plain scalar walks, where
   `size == stride`) work with no VLEN parameter. On a discontinuity (kernel
   restarts its walk) the stream counter resets but the learned indirect
   pattern is kept. Streaming entries prefetch every cache line covered by
   the next `streaming_distance` chunks of the index array.

   *Stream dedup (`stream_dedup`, default on).* The streaming window
   advances one chunk per access but spans `streaming_distance` chunks, so
   without dedup every access re-emits an almost identical window (this
   re-emission is inherited from stock IMP, `indirect_memory.cc`, where the
   window is a few lines and it is harmless). The duplicates are dropped by
   the queue filter (`pfBufferHit`) while queued, but once the original has
   issued and filled, re-emitted copies go all the way to the cache and are
   counted `pfHitInCache` → `pfLate` (`src/mem/cache/base.cc:963`,
   `prefetch/base.cc:171`) — measured on TSVC s353 this made 98% of
   "late" prefetches phantom duplicates — and with big windows they crowd
   indirect targets out of the 64-entry queue (`pfRemovedFull`). Each
   prefetch-table entry therefore keeps `streamIssuedUpTo`, the first line
   of the current walk not yet emitted: only lines at or past the mark are
   emitted, the mark advances to the window end, and it resets on a
   discontinuity so a restarted walk re-prefetches. In steady state each
   access emits only the window's leading edge (~one chunk of lines). The
   delta-mode fill-registration sweep still visits the whole window, since
   registration is gated on the entry being enabled, which may happen after
   a line was first emitted. `streamCandidatesSuppressed` counts what the
   mark filtered; set `--pf-param stream_dedup=false` for the legacy
   re-emitting behavior.
2. **Multi-index extraction.** The chunk payload is sliced into
   `index_size`-byte elements (sign-extended if `index_signed`) using the new
   `PrefetchInfo::get<T>(endian, index)` accessor — up to
   `max_indices_per_chunk` indices per event. Payloads are only present on
   hits; in steady state the index loads hit on lines VIMP itself
   streamed in (hence the class default `prefetch_on_access=True`, so hit
   accesses are observed). Note the codegen detail this relies on: TSVC-style
   gathers load raw `int32` indices (`vle32`) and scale them later
   (`vsext.vf2` + `vsll`), so the payload holds *unscaled* indices and the
   learned `shift` absorbs the scaling — exactly what IMP's shift detection
   was designed for.
3. **Alignment-free chunk IPD.** IMP's idx1/idx2 detector assumes the
   misses following an index read belong to that read's own iteration.
   That fails twice on an OoO core, both measured on `tsvc_vec` (vector
   L1D, `--debug-flags=VIMP`): (a) with several streaming loads per loop
   body, a single shared tracking slot is nearly always re-claimed by the
   wrong stream before the gather's misses arrive (5012 training episodes,
   0 detections); and (b) even with per-entry tracking, the always-hitting
   index reads race many iterations ahead of the MSHR-throttled gather
   misses, so phase-aligned correlation compares chunk *k*'s indices
   against gather *k−L*'s misses and can never match (1737 fully-compared
   episodes, 0 detections). The detector therefore drops read-clocked
   phases entirely and uses an alignment-free invariant: **two misses of
   the same gather differ by exactly `(idx_a − idx_b) << shift` for two
   distinct indices of the single chunk that produced them.** Each IPD
   entry keeps the leading `ipd_indices_per_chunk` indices of the last
   `ipd_chunk_history` chunks (the lag tolerance) plus the last
   `addr_array_len` tracked miss addresses; every element-sized (≤ 8 B)
   access feeds every valid entry concurrently (`trackMiss`), and a
   matching pair pins down `(baseAddr, shift)` in one step. Wide accesses
   never enter correlation, only PCs with a confirmed stream may allocate
   entries, and negative shifts use `applyShift` instead of stock's UB
   `index << negative`. Because a coincidental pair can falsely enable an
   entry, an enabled entry that predicts nothing for `demotion_chunks`
   consecutive chunks is demoted and retrains (`patternsDemoted` stat) —
   stock IMP's `enabled` bit is sticky forever.
4. **Batched indirect prefetch.** Once confident
   (`indirectCounter > prefetch_threshold`, maintained per chunk via
   `checkAccessMatchOnActiveEntries`), every chunk access generates
   `baseAddr + (idx << shift)` for **each** index in its payload,
   deduplicated at cache-line granularity and capped by
   `max_indirect_targets`. With a 16-element chunk this issues the whole
   iteration's gather footprint at index-load time, while the gather itself
   drains element-serially — intra-chunk lookahead stock IMP cannot express.
   The confidence check is *windowed*: stock IMP (and VIMP originally)
   matches accesses only against the latest index read, but on the OoO
   core the gather accesses that would confirm a prediction arrive
   several chunks behind the index reads — the same skew
   `ipd_chunk_history` covers during detection — so a width-1 window
   compares chunk *N+k*'s predictions against chunk *N*'s accesses
   (disjoint sets), confidence never builds, and indirect prefetching
   never activates at any threshold (observed on TSVC: enabled entries
   demoted after 16 barren chunks, `indirectCandidates = 0`). Enabled
   entries therefore keep the indices of the last
   `confidence_chunk_history` chunks (default 16, mirroring
   `ipd_chunk_history`) and match accesses against all of them; at
   detection the window is seeded from the IPD entry's chunk history so
   the in-flight misses of already-read chunks can match immediately
   instead of demoting a fresh pattern. Matches are counted by the
   `confidenceMatches` stat — if `patternsDetected` is nonzero but
   `confidenceMatches` stays 0, the learned `(baseAddr, shift)` is wrong
   (e.g. the pair invariant matched the wrong history chunk on a regular
   index array, shifting the base by a whole number of chunks).
5. **Configurable inter-chunk lookahead (`indirect_delta`).** With
   `indirect_delta=0` (default) targets come from the current chunk's own
   payload, as above. With `indirect_delta=N > 0`, targets are issued for
   chunks up to *N* ahead of the demand stream. The future index values
   only exist in the lines VIMP itself is stream-prefetching, so while
   streaming it registers each future line's *expected physical address*
   (fill packets for prefetched lines carry no VA — `createPkt` in
   `queued.cc` builds PA-only requests — so the PA is computed up front
   from the trigger's VA/PA pair, same-page only) in a small
   expected-fill table (`pending_fill_entries`). When the fill arrives,
   `notifyFill` (probe pre-connected by `Base::regProbeListeners`,
   fired in `BaseCache::recvTimingResp` after `handleFill`,
   `src/mem/cache/base.cc:610`) matches the PA, slices the line's data
   into indices, and buffers them on the stream's PT entry
   (`pending_index_sets`). Each demand chunk read then *releases* every
   buffered set within `indirect_delta` chunks of the demand pointer,
   emitting its targets through the normal `calculatePrefetch` path — so
   page-crossing targets keep the demand access's translation context
   (no synthetic packets needed). Effective lookahead is
   `min(indirect_delta, streaming_distance)` since index lines only fill
   `streaming_distance` ahead; lines past a page boundary of the trigger
   cannot be registered (unknown frame), so those chunks — at most
   `streaming_distance` out of `page_size/chunk` per page — are a small
   capture gap. Sets the demand stream has already passed (e.g. after a
   kernel restart, which also clears the buffer) are dropped as stale.
6. **Virtual-address training + MMU translation.** Class default
   `use_virtual_addresses=True`: `probeNotify` then trains on
   `pkt->req->getVaddr()` (`base.cc`), so learned bases and computed targets
   are VAs that stay linear across page frames, and `Queued::insert`
   translates page-crossing targets through the registered MMU. The rvv
   config layer wires this up: `prefetcher_factory.needs_mmu()` decides
   (vimp, or an explicit `use_virtual_addresses=true` on any prefetcher), and
   `VectorSplitCacheHierarchy(prefetcher_needs_mmu=True)` calls
   `registerMMU(cpu.core.mmu)` on the attached prefetcher. PA-mode baselines
   (stride/imp/isb/stems without the explicit param) get no MMU and keep
   their historical page-drop behavior for comparability.

### Behavior changes

- New CLI: `--prefetcher vimp` (works with the existing `--prefetcher-side`,
  `--prefetcher-level`, `--pf-param`); the config banner reports
  `PF MMU: registered …` for VA-training selections. No other prefetcher's
  behavior changes unless `use_virtual_addresses=true` is explicitly passed.
- Two target-generation modes, selected by `indirect_delta`: `0` (default)
  is the payload-immediate mode (current chunk's targets at index-load
  time); `N>0` is the fill-capture lookahead mode described above. In
  immediate mode the fill hook is inert (first-line early return), so
  `indirect_delta=0` runs are byte-identical to the original design.
- Attach point matters: at the vector L1D every demand access is observable;
  at the vector L2 the observed stream is the L1-miss stream, whose forwarded
  packets keep the original request (exact vaddr, PC, and payload on L2
  hits), so both placements train. In both cases index payloads are readable
  only when the index load *hits* the attached cache — which VIMP's own
  streaming sustains after the first few chunks.
- New stats under the prefetcher (`vimpStats.*`): `chunksSliced`,
  `indicesExtracted`, `streamCandidates`, `indirectCandidates`,
  `patternsDetected`, `patternsDemoted`, `ipdDropsNoMatch` (evictions),
  and for delta mode `fillLinesCaptured` / `pendingSetsDropped` — plus all
  inherited `Queued`/`Base` stats (`pfIssued`, `pfSpanPage`, `pfUseful`,
  …). Debug trace: `--debug-flags=VIMP` (entry allocation/eviction,
  `pattern DETECTED` with the matching miss and index pair, `DEMOTED`).

## Feature: prefetcher learned-state reset at ROI boundaries

**Files:** `src/mem/cache/prefetch/base.hh/.cc` (hook),
`stride.hh/.cc`, `indirect_memory.hh/.cc` (IMP),
`vector_indirect_memory.hh/.cc` (VIMP), `irregular_stream_buffer.hh/.cc`
(ISB), `spatio_temporal_memory_streaming.hh/.cc` (STeMS)

### What stock gem5 did

Prefetcher training state lives for the whole simulation. Nothing ever
clears a prefetcher's tables: `statistics::reset()` (triggered by the
`m5_reset_stats` / `m5_dump_reset_stats` pseudo-ops) resets *stat values*
only. The TSVC harness wraps every kernel in `ROI_BEGIN`/`ROI_END`
(`tsvc-gem5/src/common.h:33-34`), which are exactly those pseudo-ops — so
while each kernel's *stats* were isolated, the prefetcher entered every
ROI already trained by the preceding setup code (array/`ip[]`
initialization loops) and by all previous kernels. Setup is a linear
write sweep, so e.g. Stride entered each ROI with high-confidence
learned strides and IMP with a populated prefetch table, contaminating
per-kernel comparisons. IMP's cross-kernel state was also the source of
irreproducible wedges: a stale `ipdEntryTrackingMisses` pointer from a
previous kernel silently consumed all miss tracking in the next one.

### What this fork does instead

Piggybacks on the stats-reset callback mechanism gem5 already provides
(`statistics::registerResetCallback()`, `src/base/statistics.hh:2913`;
same pattern as `src/mem/probes/mem_footprint.cc:87`):

1. `Base::Base()` (`base.cc`) registers a reset callback that invokes a
   new virtual `Base::resetLearnedState()` (`base.hh`, default no-op).
   Every stats reset — i.e. every `ROI_BEGIN` and `ROI_END` — now also
   discards the prefetcher's learned state. This is unconditional (no
   config knob): each ROI always starts with a cold predictor while the
   caches stay warm.
2. Overrides clear each prefetcher's full learned state, using the
   existing `AssociativeCache::clear()` (which `invalidate()`s every
   entry and its replacement data, `src/base/cache/associative_cache.hh`):
   - **Stride:** all per-context `pcTables`.
   - **IMP:** `prefetchTable`, `ipd`, and nulls `ipdEntryTrackingMisses`
     — the pointer refers into the IPD, so it must not survive the
     clear. Side benefit: the stuck-IPD wedge (entry permanently eating
     all miss tracking) can no longer leak across ROI boundaries.
   - **VIMP:** `prefetchTable`, `ipd`, and the `pendingFillLines`
     fill-capture registrations (their entry `invalidate()` overrides
     already clear the index/chunk deques).
   - **ISB:** `trainingUnit`, both PS/SP address-mapping caches, and
     `structuralAddressCounter` back to 0 (structural address space
     restarts from scratch).
   - **STeMS:** `activeGenerationTable`, `patternSequenceTable`, the
     RMOB circular queue (`CircularQueue::flush()`), and
     `lastTriggerCounter`.

### Behavior changes

- All ROI-scoped runs with stride/imp/vimp/isb/stems now measure a
  prefetcher that trains from zero inside the ROI; results are not
  comparable with runs from before this change. Prefetchers without an
  override (e.g. tagged, AMPM) keep their state (no-op default).
- Not cleared on purpose: in-flight `Queued::pfq` /
  `pfqMissingTranslation` entries (a handful of already-generated
  prefetches from setup may still issue after `ROI_BEGIN`, and dropping
  packets mid-translation is not safe), MSHR fills already in flight,
  and cache contents (the warm-cache ROI methodology is unchanged).
- Any `m5_reset_stats` from any workload triggers the wipe — the hook is
  not TSVC-specific.
