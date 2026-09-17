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

## Removed: GDP v1 (descriptor extraction), VStride, and IMP2 (2026-07-24)

Three earlier prefetchers were deleted once the transform-chain GDP (the
section below, formerly named GDP2) superseded them:

- **IMP2** (`--prefetcher imp2`): scalar-trained, region-keyed indirect
  prefetcher — learned exact (baseAddr, shift) patterns from a manual
  vl=1 software training preamble (warm pass + spread-out train pass
  inside the ROI) and keyed them by the index array's address range.
  Its role was the "software-supplied pattern" point on the
  who-supplies-the-pattern axis (vimp = hardware learning, imp2 =
  software training, gdp = architectural extraction); GDP achieves the
  same exactness with no code changes, one-iteration arming, and
  transform-chain generality, so the preamble contract stopped earning
  its keep. Deleted: `src/mem/cache/prefetch/imp2.hh/.cc`,
  `IMP2Prefetcher`, debug flag `IMP2`, `rvv/imp2_test.c`, and its
  learned-state-reset hook.

- **GDP v1** (`--prefetcher gdp` before 2026-07-24): extracted
  {producer PC, index EEW, shift, base} descriptors via a Source Tag
  Table + arming at issue (`GatherDescriptorTable`), assumed exact
  `A[B[i]]`, and guarded that assumption with a zero-match kill
  switch. The transform-chain design replays the actual vsext/vsll/
  vadd chain instead, so the assumption (and its kill switch, and the
  `A[B[i]+c]` blind spot) disappears; on poisson3Db it beat v1 1.617x
  vs 1.434x. Deleted: `src/mem/cache/prefetch/gdp.hh/.cc` (old
  version), `src/cpu/gdp_table.hh/.cc` + `GdpTable.py` (old
  `GatherDescriptorTable`; those paths now hold the renamed
  `VectorChainTable`, see the mapping below), the v1 dispatch/arming
  hooks in `src/cpu/o3/inst_queue.cc`, and the `gdp_table` BaseCPU
  param's old type. The `StaticInst::vecMemInfo()` classification virtual
  (static_inst.hh, vector.hh) SURVIVES — the current GDP table uses
  the same channel.
- **VStride** (`--prefetcher vstride`): confidence-free vector stream
  prefetcher driven by v1's unit-stride-producer records. The current
  GDP's architectural stream walk is the same mechanism (its
  `stream_only=true` ablation is VStride, exactly), so the standalone
  prefetcher was redundant. Deleted: `src/mem/cache/prefetch/
  vstride.hh/.cc`, `VStridePrefetcher`, debug flag `VStride`,
  `rvv/vstride_test.c`.

The current-name mapping after the renames: `--prefetcher gdp` is the
transform-chain design, class `GDPPrefetcher` / `gem5::prefetch::GDP`,
debug flag `GDP`. Its CPU sideband was renamed on 2026-08-12 from
`GdpChainTable` (files `src/cpu/gdp_table.{hh,cc}`, `GdpTable.py`,
BaseCPU param `gdp_table`, hook `StaticInst::gdpInstInfo()`) to
`VectorChainTable` (`src/cpu/vector_chain_table.{hh,cc}`,
`VectorChainTable.py`, param `vector_chain_table`, hook
`StaticInst::vecMemInfo()`, debug flag `VectorChain`), because the
table is shared general infrastructure — VTyche/VTyche2, ReVeLA's
`vecMemInfo()` classification, `StreamDemoteLRURP`'s stream-page
registry and the demand-side LSQ registration all use it, not just
GDP. Stats therefore moved from `...gdp_table.*` to
`...vector_chain_table.*` (pre-rename stats.txt files keep the old
path). Old sweep rows recorded under `gdp_*`/`gdp_sap_*`/`gdp_skip*`
(v1) and `vstride_*` names are historical.

## Feature: Tyche — scalar dependency-chain indirect prefetcher (port)

A gem5/RISC-V port of the Tyche prefetcher from its published artifact
(`~/Tyche-Artifact`, a ChampSim/LoongArch implementation). Tyche roots
dependency chains at IP-stride loads, records the instructions between
the root load and dependent loads (op + one trained "constant" operand
per link), and at runtime *replays* those instructions on prefetched
values: stride-prefetch the head load, capture its fill, run the
recorded ALU ops on the loaded value, prefetch the resulting dependent
load address — so any `A[f(B[i])]` the chain ALU can express is
covered, not just `A[B[i]]`. This is the scalar-code counterpart (and
published-work baseline) to this fork's GDP: Tyche *trains* chains and
constants from repeated observation, GDP *extracts* the gather pattern
architecturally from RVV semantics.

**Files:**

- `src/cpu/tyche_table.hh/.cc`, `src/cpu/TycheTable.py` (new — the
  `TycheChainTable` SimObject: IPT + PT + DCT, the CPU-to-prefetcher
  sideband channel, same wiring pattern as `VectorChainTable`)
- `src/mem/cache/prefetch/tyche.hh/.cc` (new — the cache-side
  prefetcher: IPT training/trigger, walk registration, fill capture,
  chain replay)
- `src/mem/cache/prefetch/Prefetcher.py` (additive `TychePrefetcher`)
- `src/cpu/BaseCPU.py`, `src/cpu/base.hh/.cc` (additive `tyche_table`
  param, following `vector_chain_table`)
- `src/cpu/o3/inst_queue.cc` (additive hooks next to the GDP ones:
  scalar-instruction dispatch feed + issue-time constant capture)
- `src/cpu/SConscript`, `src/mem/cache/prefetch/SConscript`
  (registration; new `Tyche` debug flag)
- `rvv/prefetcher_factory.py`, `rvv/vector_cache_hierarchy.py`,
  `rvv/riscv-rvv-se-ara-prefetcher.py` (selection `tyche`, per-core
  table wiring, `--prefetcher-side scalar` requirement)
- `rvv/tyche_test.c` + `Makefile.tests` target (functional test)

**What gem5 provides.** Nothing comparable: gem5's stock indirect
prefetcher (IMP, `src/mem/cache/prefetch/indirect_memory.cc`) infers
`base + index*shift` correlations purely from the address stream inside
the cache, with no visibility into the instructions that computed the
addresses. The artifact's Tyche lives inside ChampSim's decode stage
with oracle access to a shadow register file and shadow memory — luxury
gem5's timing-accurate O3 model does not offer, which drove the
porting decisions below.

**Structures (artifact names kept).**

- *IPT* (32 entries): per-load-PC last address / stride / 2-bit
  confidence, LRU + prefer-low-confidence replacement. Trained by the
  prefetcher on every observed access (`observeAccess`, a verbatim port
  of `tyche.cc:228-360` including the conf_trigger rule and the
  ignore-zero-stride update). Read at dispatch (`iptConfident`,
  conf>=3) to decide whether a load roots a chain.
- *PT* (32 int arch regs): whether a register's current value descends
  from a chain, and from which DCT link. Every dispatching scalar
  instruction updates its destinations (unclassified instructions clear
  them — stale infection must not outlive the value).
- *DCT* (24 entries): per-PC chain links
  `{op, constant, back-pointer, head/formed/dense, conf}`. Exactly one
  chain-dependent register source + a known op inserts a link; a
  dependent *load* triggers the `formed` backward walk. A link replays
  only when `formed && conf==3 && dense` (`need_handle`). The table
  clears wholesale when full (artifact `clear_dct`); a generation
  counter lets the prefetcher discard walk state stranded by a clear.
- *Walk list* (16 entries, the artifact's AGQ): in-flight walk steps
  awaiting their prefetch's fill.

**Runtime walk (cache side).** On an IPT trigger the head load is
stride-prefetched `stride_distance` (32) iterations ahead; if the PC
roots a replayable chain, that target address is registered as a walk
step. Walk prefetches are virtual addresses translated by the queued
prefetcher; the `getPacket()` override peeks each deferred packet as it
issues — its PrefetchInfo holds the VA, its pkt the translated PA
(the issued request itself is PA-only, `createPkt`) — and learns the
line's *physical* address, which is what `notifyFill()` later sees. On
the fill, the element value is read from the fill payload (replacing
the artifact's shadow-memory read in `idm_load_return`), ALU links
replay via `executeAlu` (a verbatim port of `execute_alu` semantics for
the RV64 op subset), and each dependent load link emits
`link constant + propagated value` (artifact `tyche.cc:99`) as a
prefetch candidate — buffered and flushed by the next observed access,
which supplies translation context (the same pattern GDP uses). Loads
with further replayable successors register the next walk step
(multi-hop).

**Constant operands, RISC-V flavored.** The artifact samples the
non-dependent operand's *value* at decode from its shadow regfile and
trains a 2-bit stability counter on it. gem5 has no decode-time values,
so: immediate constants (I-type ops, load offsets) are taken from the
encoding at dispatch and saturate like the artifact's
`source_registers[undepend_idx]==0` shortcut does; register constants
are sampled at *issue* (all sources readable — the same hook GDP's
`armGather` uses) and trained with the artifact's exact
same-value++/different-- rule. x0 as the other operand is constant 0.

**Instruction classification with no ISA edits.** The table decodes the
raw instruction bits from `StaticInst::getEMI()` (RISC-V returns its
`machInst`): RV64I loads + FP loads (chain terminals), addi/add/sub,
shifts, and/or/xor, mul/mulw, Zba sh[123]add(.uw)/add.uw/slli.uw, and
the common RVC forms (c.ld/c.lw/c.ldsp/c.lwsp/c.fld/c.fldsp, c.addi,
c.addiw, c.mv, c.add, c.slli, c.srli, c.srai, c.andi, c.sub[w],
c.addw, c.xor, c.or, c.and). Everything else is a chain breaker.

**Deviations from the artifact (accepted, and why):**

1. The software ALU runs synchronously at fill time instead of 2-3
   cycles per op in a `cycle_operate` loop — a few cycles of optimism
   against ~200-cycle miss latencies.
2. `dense` counting happens at dispatch, not retire (no commit-stage
   hook needed; wrong-path noise is tolerated by the >115/256 test).
3. The walk list evicts its *oldest* entry when full (the artifact AGQ's
   `pop_when_full` mode) rather than dropping the newest: in gem5 a
   walk step whose prefetch never issues (line already cached, queue
   squash) has no fill to retire it, so drop-newest would wedge
   permanently.
4. A walk dies silently if its intermediate prefetch never fills; the
   artifact leaks the AGQ entry with the same net effect.
5. Branch IDM ops are omitted (dead code in the artifact: branches have
   no destination register, so its own decode guard never inserts
   them).

**Usage.** Scalar-side design — scalar instructions never reach the
vector caches, so the config script enforces the placement:

```bash
# scalar-only run
build/RISCV/gem5.opt rvv/riscv-rvv-se-ara-prefetcher.py \
    --prefetcher tyche --prefetcher-side scalar --prefetcher-level l1 \
    ... rvv/tyche_test.bin
# or as the scalar half of a dual configuration
    --prefetcher gdp --prefetcher-side vector --prefetcher-level l1 \
    --scalar-prefetcher tyche ...
```

`--pf-param` knobs: `stride_distance` (32), `stride_only` (false, the
artifact's only_stride ablation), `walk_entries` (16),
`successors_per_wakeup` (4), `max_chain_hops` (16),
`pending_target_entries` (32); on the table (fixed defaults):
`dct_entries` 24, `ipt_entries` 32, `dense_threshold` 115.

**Test.** `rvv/tyche_test.bin` (scalar, `-fno-tree-vectorize`): mode 1
`x[idx[i]]` (chain: c.ld head -> c.slli 3 -> c.add base(reg-const) ->
c.fld), mode 2 `x[idx[i]+8]` (extra addi imm link — the `+c` pattern
the removed GDP v1 could not cover, replayed exactly here), mode 0
streaming control. Expect `tyche_table.headsInserted>=1`, `linksInserted>=3`,
`constsCaptured>0`, `prefetcher.fillsCaptured>0`,
`prefetcher.chainCandidates>0` in modes 1/2.

## Feature: GDP — Gather Dataflow Prefetcher (transform-chain replay)

The Gather Dataflow Prefetcher, built on Tyche's skeleton
(`--prefetcher gdp`, vector side only; named GDP2 until 2026-07-24,
when the descriptor-extraction v1 was removed and this design took
the name — see the Removed section above). Where the removed v1
*assumed* the `A[B[i] << log2(SEW/8)]` shape and killed descriptors
when it did not hold, this design *records* the transform
instructions between the unit-stride producer load and the gather
(vsext/vsll/vadd.vi/...) and *replays* them on captured index data —
so any `A[f(B[i])]` the chain expresses is prefetched exactly,
including `+c` biases. There is no confidence, no kill switch, and no
training anywhere; arming still takes one iteration.

**Files:**

- `src/cpu/vector_chain_table.hh/.cc`, `src/cpu/VectorChainTable.py` (the
  `VectorChainTable` SimObject: PT + DCT, the CPU-to-prefetcher
  sideband, same wiring pattern as `TycheChainTable`)
- `src/mem/cache/prefetch/gdp.hh/.cc` (the cache-side
  prefetcher: architectural stream, range-based capture, replay)
- `src/mem/cache/prefetch/Prefetcher.py` (`GDPPrefetcher`)
- `src/cpu/BaseCPU.py`, `src/cpu/base.hh/.cc` (`vector_chain_table` param)
- `src/cpu/o3/inst_queue.cc` (additive hooks next to the Tyche
  ones: vector dispatch feed + issue-time base/scalar snooping)
- SConscripts (registration; `GDP` debug flag for the prefetcher,
  `VectorChain` for the table);
  `rvv/prefetcher_factory.py`, `rvv/vector_cache_hierarchy.py`,
  `rvv/riscv-rvv-se-ara-prefetcher.py` (selection, per-core wiring —
  the gdp param is `link_table`, distinct from tyche's `chain_table`
  so the dual-config `_params` guards stay unambiguous)

**CPU side (`VectorChainTable`), hardware widths in mind:**

- *PT*: 32 vector arch regs -> {depend, 3-bit DCT ptr}. Forward
  propagation at dispatch; same source-classification rules as v1's
  STT (v0-mask skipped, in-place transforms extend the chain).
- *DCT* (8 entries, wholesale clear-on-full + generation counter):
  heads (unit-stride loads, EEW), transform links (op + scalar
  operand), gather links (base). Transform ops are decoded from the
  raw OP-V encoding via `StaticInst::getEMI()` (the tyche-port
  technique — no ISA edits): vadd/vsub, vsll/vsrl/vsra, vmul.vx,
  vsext/vzext (vrsub and vand/vor/vxor were dropped 2026-09-02, see
  "VectorChainTable: affine-only transform decode") (source width from the vtype
  vsew gem5 bakes into ExtMachInst). `.vi` immediates are valid from
  the encoding; `.vx` scalars and gather bases are snooped at *issue*
  off the operand read the instruction is already doing (armGather
  precedent) and refreshed every execution. A tagged value flowing
  into an undecodable op breaks the chain (chainsBroken).
- *Gather-link state (the former GLT, eliminated 2026-07-23)*: lives
  in the DCT's head rows as {consumer ptr (-1 = unlinked), EEW} — the
  GLT was direct-mapped 1:1 to DCT slots, i.e. just extra columns for
  head rows, and every other field proved redundant (valid ≡ DCT
  head+valid; linked ≡ consumer present; pipeline ownership belongs
  as producer tags on the pipelines). The gather's dispatch runs
  *backward propagation* — walk back-pointers to the head, write the
  consumer there. Done at dispatch because compilers alias vd with
  vs2 (v1 bug #2 lesson). Re-walked every gather dispatch, so links
  self-heal; the consumer pointers clear with their DCT rows, so a
  wholesale clear leaves no dangling pointers. Hardware note: the
  consumer field is sim-only bookkeeping (the lazy walk's anchor);
  the hardware walk starts at the gather and holds this edge only
  transiently. CPU side = PT + DCT, two structures.
- *Field naming/width pass (2026-07-22)*: DCT flavor flags are `head`
  / `tail` (tail = the gather terminating the chain); the operand
  trio is `scalar` / `scalarType` (true = encoding immediate, false =
  a register value snooped at issue) / `scalarReady` (the operand has
  arrived — set at insert for immediates, captureScalar for .vx,
  armBase for bases). *Renamed 2026-08-31 from `immType` / `immValid`:
  the slot also holds gather BASE addresses and `vlse` strides, which
  are never immediates, so the old names described only one of its
  three producers. `DecodedVArith::immType` keeps its name — at decode
  time it genuinely does mean "this operand is an encoding
  immediate".* `lastDctPtr` is 3 bits in hardware: heads have no
  predecessor, but every walker terminates on the `head` flag before
  following the pointer, so a head's pointer bits are don't-care.
  The dead per-DCT-entry EEW copy was deleted (the GLT holds the only
  copy; direct-mapped 1:1, so the two could never disagree).

**Cache side (`GDP`):**

- *Architectural stream* per producer PC (high-water dedup, restart
  on backward jump, no confidence — unit-stride identity comes from
  the opcode, so the walk is exact from the first access).
- *Chain dispatch, walk-on-demand* (revised again 2026-07-22,
  superseding the latched-in-GLT scheme): the table stores NO copy of
  the pipeline configuration. When a chunk event at a GLT-linked
  producer adopts, `pipelineConfig()` walks the chain consumer->head
  on demand (bounded by dct_entries) and the adopted copy in the
  prefetcher's Stream Tracking Table (STT) entry is the only latched
  one. Hardware reading: the backprop walk distributes each link's op
  straight into the cache-side pipeline stage registers — DCT + stage
  registers are the only copies, shrinking the GLT from ~340 to ~10
  bits/entry. Side effect: operands read at adoption are the freshest
  snooped values (the old latch's one-iteration .vx refresh lag is
  gone). Per-producer pipelines, capped by `pipelines`; the walk only
  returns valid once every scalar/base has been snooped
  (chainNotReady counts triggers that found none). The prefetcher's
  structures carry paper names now: STT (was pcStates; fields
  `valid`/`lastAddr`/`limitAddr`/`configured`), Index Routing Table
  IRT (was pendingCaptures, then briefly Prefetch Status Table PST —
  fill-capture registrations whose producerPC routes a matching
  fill's payload to that producer's slice buffer; the rename also
  un-collides with upstream STeMS's Pattern Sequence Table PST),
  Indirect Target Buffer ITB (was pendingTargets). New sizing stats:
  `itbPeakOccupancy` (high-water) and `itbEvictions` (FIFO drops,
  previously silent) justify quoting a hardware ITB depth smaller
  than the sim's 64. The generation compare at fill is documented as
  the lazy sim stand-in for a hardware invalidate pulse (zero stored
  bits).
- *Range-based capture* (no per-line watch tags, so nothing can
  wedge): a prefetch issuing inside a configured producer's stream window —
  the `getPacket()` override peeks the deferred packet, whose
  PrefetchInfo holds the VA and whose pkt the translated PA (the
  issued request itself is PA-only) — or a
  demand miss at the producer PC (VA+PA from the access) registers the
  line; `notifyFill` matches the PA and captures the payload. Because
  captures come from the stream running `streaming_distance` ahead,
  indirect targets inherit the stream's lookahead. Full timeliness
  needs distance x chunk-cadence >= TWO memory round-trips (index line
  and target line), so gdp wants larger distances than v1's stream
  (16-32 at DRAM latencies).
- *Replay (v2.1, 2026-07-23 — stall-on-full, demand-clocked)*: a
  captured fill's payload is latched into a free entry of the
  producer pipeline's slice buffer — each replay pipeline has one
  slice buffer of `slice_buffer_entries` line entries (each entry =
  512 b payload + VA tag + drain cursor + valid), and a fill arriving
  with the buffer full is dropped — bufferBusyDrops, chunk-granular
  shedding that matches vector-gather completion semantics. The drain runs at every observed access
  (`drainReplay`): slice at producer EEW (zero-extended; explicit
  vsext/vzext links handle sign), stream elements through the latched
  ops + base adder, canonical-VA filter, per-payload line-dedup, and
  emit into free prefetch-queue slots (room = queue_size − pfq −
  pending-translation − this event's candidates) with a small
  per-event floor (drainFloor=8) of bounded displacement when the
  queue is full — the floor models hardware's CONTINUOUS queue drain,
  which gem5's pull-only queue lacks (cache pulls starve exactly when
  misses rise; a pure zero-room stall wedged the drain until the
  staleness abort discarded everything — the first-cut v2.1
  poisson3Db collapse, 13.9M cycles). When the budget runs out the
  payload waits latched and resumes next access (replayDeferred),
  mirroring Ara's addrgen stall-not-drop policy. A STALENESS ABORT
  (stalenessAborts) flushes slice-buffer entries whose index line the
  walk cursor has passed (strict <, so demand-miss captures of the
  current chunk survive). Stream candidates are NEVER gated (lesson from the same
  collapse: they are regenerable state and the engine's fuel —
  gating them starves captures in a positive-feedback loop). The former Indirect Target
  Buffer list and the timed drain model (`drain_pipe_depth`,
  `pending_target_entries` params) are deleted — the hardware ITB is
  a single conversion-stage output register (48 b), and buffering
  lives in the slice buffers where a 512 b payload is denser than its
  expanded targets.
- *Replay width (`pipelines_per_gather`, 2026-07-29)*: how many
  indices of the head slice-buffer entry convert per drain event, one
  per pipeline. See the subsection below.

### Replay width: `pipelines_per_gather`

Everything else in the replay model is a *capacity* bound — queue
slots (`room`), slice-buffer entries, IRT entries. Until 2026-07-29
there was no *throughput* bound at all: `drainReplay` walked elements
until the queue budget ran out, so a whole 64 B index line could
convert in one event no matter how many transform stages the chain
had. That contradicted this section's own hardware story (a single
1-elem/cycle conversion pipe) and it made GDP and VTyche
incomparable on the axis VTyche was built to test — VTyche converts
a whole line *at once* through `blkSize/elemBytes` parallel
shift-and-add lanes (`vector_tyche.hh`), and GDP was silently getting
the same throughput for free while paying for a serial multi-stage
chain.

`pipelines_per_gather` replicates the transform-op chain + base adder
N times behind **one** gather, all fed from the same slice-buffer
entry, so N consecutive indices convert together
(`gdp.cc:188-195`, `:231-234`, `:236-247`, `:266-272`):

- **Per gather, not global.** The pipelines are private to the
  producer that owns them, so the budget is re-armed for each STT
  entry in the drain loop. One gather exhausting its pipelines does
  **not** stop another's drain — unlike `room`, which is the shared
  prefetch queue and breaks out of the whole loop (`blocked`). This is
  the structural difference between the two budgets and the reason
  they are tracked separately.
- **Filtered and deduplicated elements still cost a pipeline.** They
  went through the datapath; only elements that survive to an emitted
  target consume a queue slot (`room--`). So `elementsReplayed` is the
  width-budget consumer and `targetsGenerated` the queue-budget
  consumer, and the two diverge exactly by the filter+dedup rate.
- **`replayWidthLimited`** counts drain events in which some gather's
  pipelines ran out with elements still buffered. Read it against
  `replayDeferred` (queue-bound events) to see which budget actually
  binds: if width never binds, N is above the working rate and costs
  nothing; if it dominates, the chain is the bottleneck rather than
  the queue.
- **Default 0 = unbounded**, i.e. the pre-2026-07-29 behavior, so all
  earlier GDP results (including the 1.617x poisson3Db row) remain the
  baseline and reproduce unchanged. Any N >= 1 is a strictly tighter
  model.

Calibration points worth sweeping: **N = 1** is the strictly serial
pipe the design text originally claimed; **N = blkSize/elemBytes**
(16 for a 64 B line of 32-bit indices) is one whole line per event,
which is VTyche's conversion width — at that setting a GDP-vs-VTyche
delta is *only* the transform algebra, with conversion throughput held
equal. Intermediate N answers the design question directly: how much
replay width does the transform-chain approach need before its
generality stops costing timeliness?

**Approximations (accepted, why):** replay computes at 64 bits (exact
for the widening index chains compilers emit; SEW<32 wraparound
differs), masked (vm=0) transforms ignore v0 (over-prefetch, never
corruption), target emission is demand-clocked (v2.1: drained at the
next observed access rather than at a modeled per-element pipeline
rate — so `pipelines_per_gather` meters elements per *drain event*,
not per cycle; events are demand-clocked, which is the coarser and
more conservative clock), and wrong-path dispatches update the tables
(self-healing via the per-gather re-walk).

**Validation:** no new test binary needed — `rvv/gdp_test.bin` covers
both paths. Mode 1 (`x[idx[i]]`): expect `vector_chain_table.linksFormed>=1`,
`prefetcher.chainDispatches>0`, `fillsCaptured>0`,
`targetsGenerated>0`. Mode 0 (`x[idx[i]+8]`, the pattern v1 KILLS):
the `vadd.vi v1,v1,8` becomes a recorded Add link — expect the same
healthy stats (gdp has no kill switch), i.e. mode 0 flips from killed
(the removed v1) to prefetched exactly. That flip is the design's
thesis in one run.

```bash
build/RISCV/gem5.opt rvv/riscv-rvv-se-ara-prefetcher.py \
    --prefetcher gdp --prefetcher-side vector --prefetcher-level l1 \
    --vlen 512 --vector-timing-throughput 4 --simd-units 2 \
    -p "0" rvv/gdp_test.bin
```

---

## Feature: VTyche — Vector Tyche (A[B[i]] gathers, collapsed to a linear equation)

`--prefetcher vtyche`, vector side only. A deliberate hybrid: GDP's
*architectural discovery* with IMP's *linear equation* for generation.
It shares GDP's entire CPU side — the same `VectorChainTable` SimObject,
the same producer identification, producer→gather link and base snoop
— but replaces the transform-replay pipeline with

```
target = base + (index << shift)
```

evaluated by one shift-and-add lane per sliced index, all lanes in the
same event. The chain is not replayed at runtime; it is **collapsed
once**, at adoption, into that `(base, shift)` pair.

Positioned against the two designs it borrows from: IMP *learns*
`(base, shift)` from miss-address deltas and needs a confidence ramp,
demotion, and a wrong-base escape hatch; GDP *records and replays* the
whole transform chain and pays for a per-stage pipeline plus a serial
per-element drain. VTyche takes IMP's equation but reads its operands
architecturally, so there is no training, no confidence, and no kill
switch — and it takes GDP's discovery but throws away everything the
equation cannot express.

**Files:**

- `src/mem/cache/prefetch/vector_tyche.hh/.cc` (the whole prefetcher;
  no new CPU-side code — `VectorChainTable` is reused unmodified)
- `src/mem/cache/prefetch/Prefetcher.py` (`VectorTychePrefetcher`; the
  param is `link_table`, the *same name* GDP uses, so the per-core
  wiring in `rvv/vector_cache_hierarchy.py` — which guards on
  `"link_table" in prefetcher._params` — picks it up with no change)
- SConscripts (registration; `VTyche` debug flag)
- `rvv/prefetcher_factory.py` (`PREFETCHERS`, `VA_PREFETCHERS`,
  `VECTOR_CHAIN_TABLE_PREFETCHERS`), `rvv/riscv-rvv-se-ara-prefetcher.py`
  (`--prefetcher vtyche`)

### What GDP does

`GDP::calculatePrefetch` (`gdp.cc:300-318`) adopts a
`VectorChainTable::ChainSnapshot` — a *list* of `StageOp{op, scalar,
extWidth}` in head-to-gather order — into the producer's STT entry.
At drain time `GDP::applyChain` (`gdp.cc:131-165`) walks that list per
element, and `drainReplay` (`gdp.cc:167-254`) walks the elements
serially across access events, bounded by `drainFloor = 8` emissions
per event. The captured raw payload waits in the slice buffer with a
`nextElem` cursor into undecoded bytes.

### What VTyche does instead

**1. The collapse (`collapse()`, `vector_tyche.cc:100-186`).** At
adoption the snapshot is folded into a `LinearForm{base, shift,
extBits, extSigned}` by tracking the running affine form
`(elem << shift) + bias`:

| Chain op | Folded as |
|---|---|
| leading `vsext`/`vzext` (at most one) | the slice's extension width + sign — not a stage; this is how the element is *read* |
| `vsll` | `shift += k`, `bias <<= k` |
| `vmul` by a power of 2 | `shift += ctz(c)`, `bias *= c` (a power-of-2 multiply *is* a shift) |
| `vadd` / `vsub` scalar | `bias ± c` |
| gather base (`rs1`) | `base = snap.base + bias`, folded once |
| `vrsub`, `vsrl`, `vsra`, `vand`, `vor`, `vxor`, general `vmul` | **rejected** (`chainsRejected`) |

An extend that appears *after* anything has scaled or biased the value
is also rejected: it is then a genuine mid-chain operation, not a slice
width. Compilers emit at most one, so a second is rejected rather than
composed.

**Why this is lossless for `A[B[i]]`.** RVV indexed loads take *byte
offsets*, so source-level `A[B[i]]` compiles to
`vle32 → vsext.vf2 → vsll.vi #k → vluxei64` — the chain GDP replays is
already exactly an extend, a shift, and the gather's base add. Folding
it yields the same addresses GDP computes. `A[B[i]+c]` and `A[B[i]*8]`
fold too, because the base adder and the shifter are there either way.
What is given up is genuinely everything beyond an affine index→address
map, and it is rejected outright rather than approximated — so the gap
against GDP on the same kernel is *exactly* the transform pipeline's
contribution.

**2. Conversion at the fill, in parallel (`convertLine()`,
`vector_tyche.cc:246-305`).** GDP holds the payload and decodes it
element-serially at drain. VTyche converts the *whole line* the moment
it is captured (`notifyFill`), one shift-and-add lane per element —
`blkSize / EEW` lanes, 16 for a 64 B line of 32-bit indices. What
enters the queue is a line-deduplicated list of finished target
addresses, so the structure that waits for queue room is an **address
queue**, not a payload buffer, and there is no cursor into undecoded
bytes. This is the structural difference between the two designs.

**3. Emission, demand-clocked and stall-on-full (`drainEmission()`,
`vector_tyche.cc:307-370`).** Unchanged in discipline from GDP: a
converted batch releases into free prefetch-queue slots at the next
observed access (which supplies the translation context fills lack),
what does not fit waits in the batch (`emissionDeferred`), a staleness
abort flushes batches whose index line the walk cursor has passed
(`stalenessAborts`), and a fill arriving with the address queue full is
dropped whole (`bufferBusyDrops`, chunk-granular shedding). The
`drainFloor` bounded-displacement floor is kept — the v2.1 poisson3Db
collapse showed a pure zero-room stall wedges emission exactly when
miss pressure is highest — but it is now the `drain_floor` **param**
rather than a hardcoded constant, so `drain_floor=0` gives the pure
stall for comparison. Stream candidates are never gated, same as GDP.

Everything else — the architectural stream walk (`streamAhead`),
range-based capture via the IRT, the `getPacket()` VA→PA peek, the
demand-miss capture path, the generation compare standing in for a
hardware invalidate pulse — is GDP's, structurally unchanged, so the
two are directly comparable run-for-run.

**One second-order difference falls out of converting early.** GDP
evaluates the base at *drain* time (`ps.config.base`, refreshed at each
adoption), so a batch that sits through a re-arm is emitted against the
newer base. VTyche binds the base at *fill* time, which matches the
iteration the captured index data actually came from. VTyche is the
more correct of the two here; it only shows up when an outer loop
re-arms the gather base while an index line is still queued.

### Storage

Per configured pipeline the latched "configuration" is two operands
(`base` 48 b + `shift` 6 b + `extBits` 7 b + sign 1 b ≈ 62 b) instead
of GDP's stage-register file, and the address queue holds
`address_queue_entries` batches of up to `blkSize/EEW` 48-bit
addresses. Against GDP the trade is: no per-stage registers and no
payload buffer, but expanded addresses are less dense than the 512 b
payload they came from when dedup does not bite.

### Parameters

| Param | Default | Meaning |
|---|---|---|
| `link_table` | NULL | the per-core `VectorChainTable` (wired by the config script) |
| `streaming_distance` | 8 | index-array stream lookahead in lines; also sets the indirect lookahead |
| `stream_only` | False | ablation: stream but never capture/convert |
| `address_queue_entries` | 2 | converted batches held per pipeline |
| `pipelines` | 2 | concurrently configured producers |
| `routing_entries` | 32 | IRT capacity |
| `retired_tail_entries` | 32 | retired-IRT tail depth (converted lines remembered after they leave the pending IRT); 0 disables |
| `capture_read_hit_own_producer` | True | read-hit capture of a line this producer already converted: True = capture anyway (pre-tail behaviour), False = suppress |
| `drain_floor` | 8 | minimum emissions per event when the queue is full; 0 = pure stall |

### Behavior changes

None to any existing configuration: this is an additive prefetcher
selection. `--prefetcher vtyche` requires `--prefetcher-side vector`
(same gate as gdp, via `VECTOR_CHAIN_TABLE_PREFETCHERS`) and is not accepted for
`--scalar-prefetcher`. It trains on virtual addresses, so the CPU MMU
is registered automatically.

### Validation

No new test binary — `rvv/gdp_test.bin` covers both shapes. Run
2026-07-28, vlen 512 / throughput 4 / simd-units 2, prefetcher on the
vector L1:

| | mode 0 (`x[idx[i]+8]`) | mode 1 (`x[idx[i]]`) |
|---|---|---|
| `vector_chain_table.transformsInserted` | 2 | 1 |
| `vector_chain_table.linksFormed` | 1 | 1 |
| `formsAdopted` | 1709 | 1744 |
| **`chainsRejected`** | **0** | **0** |
| `fillsCaptured` | 1298 | 1326 |
| `elementsConverted` | 10384 | 10608 |
| `targetsGenerated` | 8071 | 8071 |

**The collapse is exact: vtyche and gdp are bit-identical on every
headline stat, in both modes** — same `numCycles` (5,125,523 /
5,125,524), same `pfIssued` (7350 / 7002), same `pfUseful` (3064 /
2419), same accuracy, coverage and L1 miss rate. Replaying the recorded
chain per element and evaluating the collapsed `base + (index << shift)`
produce the same addresses at the same times, which is exactly the
claim. Mode 0 is the sharper of the two: its `vadd.vi v1,v1,8` becomes
an Add link that vtyche **folds into base** rather than replaying, and
`chainsRejected` stays 0.

Neither mode exercises the rejection path; a chain with a
non-power-of-2 `vmul` or a right shift is needed to see
`chainsRejected` move, and on such a kernel vtyche should fall back to
stream-only behavior while gdp still prefetches indirectly. That
divergence is the intended ablation and is currently untested — no
kernel in `rvv/` produces such a chain.

### Sweep: RiVec spmv, poisson3Db (2026-07-28)

7 configs via `rvv/spmv_param_sweep.py --only vtyche`, anchored on the
best GDP row. Filed to the `VTYCHE` sheet of
`rivec_results/poisson3Db_params.xlsx`. Baseline (no prefetcher)
14,615,655 cycles.

| Config | sd | aq | df | Cycles | Speedup | VL1 miss | Acc | Cov |
|---|---|---|---|---|---|---|---|---|
| `vtyche_sd16_aq8` | 16 | 8 | 8 | 9,031,129 | **1.618×** | 0.1475 | 0.703 | 0.856 |
| `vtyche_sd16_aq4_df0` | 16 | 4 | 0 | 9,077,274 | 1.610× | 0.1550 | 0.699 | 0.841 |
| `vtyche_sd16_aq4` | 16 | 4 | 8 | 9,099,629 | 1.606× | 0.1497 | 0.702 | 0.846 |
| `vtyche_sd24_aq4` | 24 | 4 | 8 | 9,130,765 | 1.601× | **0.1097** | 0.715 | **0.862** |
| `vtyche_sd32_aq4` | 32 | 4 | 8 | 9,339,528 | 1.565× | 0.1474 | 0.648 | 0.797 |
| `vtyche_sd16_aq2` | 16 | 2 | 8 | 9,468,737 | 1.544× | 0.1771 | 0.709 | 0.783 |
| `vtyche_sd16_aq2_streamonly` | 16 | 2 | 8 | 10,728,226 | 1.362× | 0.5081 | 0.723 | 0.274 |

**`chainsRejected == 0` in every row.** spmv's chain is
`vle64 → vsll.vi #3 → vluxei64` (`ja[]` is `uint64_t`, so no extend is
emitted), which collapses to `base + (idx << 3)` — the affine form
holds, so vtyche never falls back.

**Head-to-head with GDP:**

- **Best-to-best is a tie**: vtyche 1.618× (9,031,129) vs gdp 1.618×
  (9,034,641, `sd16_sb8`) — vtyche ahead by 0.04%, i.e. nothing. The
  collapsed equation buys no hit rate over full chain replay on an
  affine kernel, exactly as `gdp_test` predicted. Its case is hardware
  cost, not performance.
- **The stream-only ablation is bit-identical** — both 10,728,226
  cycles, both 227,335 `pfLate`. Confirms the shared stream/capture
  machinery is genuinely unchanged; every divergence below comes from
  the convert/emit path alone.
- **At matched queue depth vtyche sheds more fills.** `aq4` vs `sb4`:
  10,577 vs 1,290 `bufferBusyDrops` (+720%), 3.9% fewer targets, 0.66%
  more cycles. `aq2` vs `sb2`: +13% drops, 1.36% more cycles. VTyche
  needs one depth step more (aq8) to reach GDP's `sb4` result. A
  converted batch occupies its slot as one unit, so occupancy is
  coarser than GDP's payload buffer — but the 720% gap is larger than
  that reasoning alone predicts and is **not fully explained**; the
  emission-order difference feeds back into fill arrival timing, so
  the two are not cleanly separable. Treat aq8 as the operating point,
  not aq4.

**`drain_floor=0` does not collapse.** The pure stall-on-full ran
9,077,274 cycles — 0.25% *better* than the same config with the
floor — while `emissionDeferred` rose 29× (10,519 → 301,160). The
GDP v2.1 poisson3Db collapse that motivated keeping the floor
(`gdp.cc:170-176`) does not reproduce for vtyche on this workload, so
the caution behind defaulting `drain_floor=8` was not borne out here.
The default stays at 8 for comparability with GDP's hardcoded value;
`--pf-param drain_floor=0` is validated safe and marginally faster on
this kernel.

```bash
python3 rvv/spmv_param_sweep.py --input poisson3Db --only vtyche --jobs 4
```

```bash
build/RISCV/gem5.opt rvv/riscv-rvv-se-ara-prefetcher.py \
    --prefetcher vtyche --prefetcher-side vector --prefetcher-level l1 \
    --vlen 512 --vector-timing-throughput 4 --simd-units 2 \
    -p "0" rvv/gdp_test.bin
```

### Cross-line dedup window (`dedup_buffer_size`)

**What gem5/vtyche did before.** Deduplication existed at exactly one
scope: within a single converted batch. `convertLine()`
(`src/mem/cache/prefetch/vector_tyche.cc`) drops a target whose line
another element of the *same* index line already produced
(`targetsDeduplicated`), and `drainEmission()` released as many
buffered batches per access event as queue room allowed. Nothing
deduplicated *across* index lines, and the measured cost was visible
in the standard prefetcher stats: on poisson3Db ~24% of all candidates
were dropped by the cache as already-resident (`pfHitInCache`,
incremented in `src/mem/cache/base.cc` when an issuing prefetch finds
its line in the tags) — overwhelmingly neighboring index lines
re-proposing the same target lines, which the per-batch scope cannot
see.

**What was added.** An optional per-producer cross-line dedup window,
`--pf-param dedup_buffer_size=N` (default 0 = off, behavior unchanged;
`src/mem/cache/prefetch/Prefetcher.py`). Each target's line is checked
at emission against the lines *actually emitted* by that producer's
last N processed index lines (`SttEntry::dedupWindow`, a FIFO of
per-line contribution lists in `vector_tyche.hh`). A hit drops the
target (`targetsCrossDeduplicated`) without consuming a queue slot.
Semantics are first-emit-wins: a dropped target was pushed by an
earlier line still inside the window, so coverage is never silently
lost. Only *emitted* lines enter the window — targets that were
themselves deduped, filtered, or staleness-aborted do not refresh it.
N sets only the window depth: the serial line cadence a CAM scan
would cost is already imposed by the one-line-per-event conversion
(see the redesign note below), so the dedup adds no extra pacing.

Contributions are accumulated in `ConvertedLine::emitted` so a latch
stalled mid-emission by queue room resumes with its window bookkeeping
intact, and a processed line enters the window even when its
contribution is empty (the processing slot is spent either way). The
window clears on a restarted walk (backwards jump in
`calculatePrefetch`) — after a restart its lines may have been evicted,
and suppressing re-emission would punch holes in the new pass.

**Hardware cost.** Per pipeline: N × (blkSize/EEW) line addresses in a
FIFO CAM (N=8, 16 lanes ≈ 128 tags) plus one comparator column; the
one-batch-per-event pacing is what the serial CAM scan costs in time.

**Untested.** No sweep has run with N > 0 yet; the hypothesis to test
is that `targetsCrossDeduplicated` converts mostly one-for-one from
`pfHitInCache` drops, freeing queue slots and issue bandwidth on
poisson3Db-class matrices where neighboring index lines share target
lines.

### Redesign (2026-07-30): raw slice buffers, conversion at the drain

**What vtyche did before.** Conversion ran *at the fill*
(`notifyFill` → `convertLine`): the shift/add array was modeled at
the fill port, so what waited for queue room was an **address queue**
of finished, per-line-deduplicated target batches
(`address_queue_entries`), and emission could release any number of
batches per access event. Structurally this diverged from GDP in two
ways beyond the intended one — eager compute (a batch later flushed
by the staleness abort had already paid full conversion) and coarser
buffer occupancy (the documented aq4-vs-sb4 `bufferBusyDrops`
asymmetry).

**What changed.** VTyche now stages **raw payloads in slice buffers
exactly as GDP does** (`slice_buffer_entries`, renamed from
`address_queue_entries` — sweep results before this date used the old
name and the old semantics). At the drain, ONE buffered line per
producer per event converts through the parallel lane array into a
single **output latch** (`SttEntry::latch`), whose targets release
into free queue slots; the array cannot convert the next line until
the latch empties, and running out of the one-line slot with lines
still buffered counts `emissionLineLimited`. Stale lines flush from
the slice buffer *before* any compute is paid, matching GDP's
laziness.

The point of the restructure: the two designs now share staging,
occupancy semantics, lazy staleness, and the demand-clocked release —
the **only** remaining difference is the drain's compute engine.
GDP walks the payload at `pipelines_per_gather` elements per event
through the replay chain; vtyche converts a whole line per event
through the affine lane array (`blkSize/EEW` shift-and-add lanes). A
GDP run at `ppg = blkSize/EEW` (e.g. ppg8 for e64) is now the exact
width-matched control for vtyche.

Files: `src/mem/cache/prefetch/vector_tyche.{hh,cc}` (CapturedLine,
SttEntry::sliceBuffer/latch, convertLine, drainEmission, notifyFill),
`src/mem/cache/prefetch/Prefetcher.py` (param rename),
`rvv/spmv_param_sweep.py` (VTYCHE_KEYS). Pre-redesign vtyche numbers
(the 2026-07-28 sheet) are not directly comparable to post-redesign
runs: conversion timing, shedding pressure, and emission cadence all
moved.

## Feature: unified (non-split) cache hierarchy with prefetcher hosting (2026-08-01)

**What gem5 provides.** The stdlib
`PrivateL1PrivateL2CacheHierarchy`
(`src/python/gem5/components/cachehierarchies/classic/private_l1_private_l2_cache_hierarchy.py`)
gives each core a private L1I/L1D/L2 chain with every data access —
scalar and vector alike — through the one L1D. It exposes no
prefetcher or MSHR knobs, and the stdlib cache classes it instantiates
(`.../classic/caches/l1dcache.py`, `l2cache.py`, `l1icache.py`) each
attach a **StridePrefetcher by default** (`PrefetcherCls`
constructor default), so a stock run of this hierarchy silently
prefetches on every cache — it is not a usable no-prefetcher
baseline, and there was no way to host this fork's prefetchers on it:
the run script's `--prefetcher` flag force-enabled `--vector-cache`
(the split hierarchy) because only
`rvv/vector_cache_hierarchy.py` had the attach/MMU/sideband wiring.

**What this fork adds.** `rvv/unified_cache_hierarchy.py` —
`UnifiedCacheHierarchy(PrivateL1PrivateL2CacheHierarchy)` — replicates
the parent's `incorporate_cache` wiring (no VectorSplitter, one shared
chain) and grafts on the same prefetcher hosting the split hierarchy
has, copied from `VectorSplitCacheHierarchy`:

- every cache's default StridePrefetcher is set to `NULL`, then one
  optional zero-arg factory (from `rvv/prefetcher_factory.py`)
  attaches to the L1D or the L2;
- `_finish_prefetcher` is identical to the split class's: MMU
  registration for VA-training prefetchers, and per-core
  `TycheChainTable` / `VectorChainTable` sideband tables wired to both
  the prefetcher (`chain_table` / `link_table`) and the core
  (`tyche_table` / `vector_chain_table`);
- `mshrs` / `tgts_per_mshr` constructor knobs on L1D and L2, defaults
  matching the stdlib classes (16/20 and 20/12).

Run-script side (`rvv/riscv-rvv-se-ara-prefetcher.py`): new
`--unified-cache` flag, mutually exclusive with `--vector-cache` and
`--scalar-prefetcher`. With it, `--prefetcher` no longer forces the
split hierarchy; `--prefetcher-side` is ignored (one chain) and
`--prefetcher-level` picks L1D vs L2; `--l1d-mshrs`/`--l2-mshrs` (and
tgts) are honored. The gdp/vtyche "side must be vector" and tyche
"side must be scalar" guards are skipped in unified mode: those guards
exist because the VectorSplitter hides one access class from each
chain, and the unified L1D sees both. This was validated on
spmv/football: vtyche on the unified L1D adopts forms and the CPU-side
`vector_chain_table.linksFormed` fires, with scalar accesses passing through
vtyche's PC gate harmlessly (`vector_tyche.cc` `calculatePrefetch`
early-returns when `tbl->producerInfo(pc)` finds nothing).

Purpose: measure prefetchers in a conventional hierarchy where scalar
traffic shares the caches and MSHRs with vector traffic, as a control
against the split-hierarchy results in `rivec_results/`
(unified-hierarchy runs are filed there with a `_unified` label
suffix).

## Feature: `--mem-type` — perfect-memory counterfactual (2026-08-04)

### What stock gem5 provides

The gem5 standard library ships multiple interchangeable memory
systems behind `AbstractMemorySystem`: full DRAM timing models
(`gem5.components.memory.SingleChannelDDR4_2400` — a `MemCtrl` with
read/write queues and FR-FCFS scheduling in front of a `DRAMInterface`
modeling banks, tRCD/tCL/tRP, one 64B burst per tBURST=3.332ns =
19.2GB/s peak, refresh, turnaround) and a fixed-latency stub
(`gem5.components.memory.simple.SingleChannelSimpleMemory`,
`src/python/gem5/components/memory/simple.py:63` — a `SimpleMemory`
with a configured latency/latency_var/bandwidth and no controller,
banks, or queueing; its own docstring markets it "for studies that do
not target memory subsystem design"). The run script previously
hardcoded DDR4-2400.

### What this fork adds

`rvv/riscv-rvv-se-ara-prefetcher.py` grows a `--mem-type
{ddr4,perfect}` flag (default `ddr4`, behavior unchanged). `perfect`
swaps in `SingleChannelSimpleMemory(latency="1ns", latency_var="0ns",
bandwidth="1TiB/s", size="8GiB")`: every access past the membus is
serviced in 1 cycle at effectively unlimited bandwidth. Everything
above the membus (caches, xbars, MSHRs, prefetchers, coherence) is
untouched, so misses exercise the same machinery and simply return
almost instantly.

Purpose: measure a kernel's **compute floor** — its runtime when
memory is never the limiter. CORRECTION (same day, from the 64MiB-L1
run below): the initial rationale claimed a huge cache cannot warm
the fread-loaded RiVec arrays because gem5-SE services `fread`
functionally. That is only half true: the read() syscall fills the
stdio buffer functionally, but newlib's fread then memcpy's each
chunk to the destination array with real simulated loads/stores —
every line of `a`/`ja`/`ia` IS touched through the timing caches
pre-ROI (evidence: the ~206M-cycle pre-ROI phase, the snoop filter
overflowing pre-ROI at 8MiB tracked, and the ROI running 99.7%
L1-hit in a 64MiB L1). So big-cache warming DOES work for these
benchmarks; `--mem-type perfect` remains useful as the
latency-counterfactual and for bounding compulsory-miss cost.

Caveat for interpreting results: `SimpleMemory` is not a
`DRAMInterface`, so `mem_ctrl.dram.*` stats (busUtil, avgQLat,
rowHitRate) do not exist in a `perfect` run.

### First measurement (poisson3Db spmv, unified, L1 mshrs=32/L2=40)

base+perfect = 9.70M cycles (vs 15.01M on DDR4): free DRAM recovers
only a third of base's stall time because 2.31M L1 misses still pay
the ~38ns L1→L2 path, exposed through the 32-entry LQ.
vtyche+perfect = 8.06M cycles: the practical compute floor; even
95.6% coverage + 1ns memory leaves LQ-full rename blocks (887K) and
~325K L1 misses. Since vtyche+DDR4 already runs at 8.82M, the
prefetcher sits within ~9% of this ceiling — the DRAM-bandwidth floor
(3.4M cycles) is unreachable on this core because the LSQ/vector side
saturates first.

### Exact compute floor: 64MiB L1D + perfect memory (2026-08-04)

`--l1d 64MiB --mem-type perfect` (base, unified, mshrs 32/40) holds
the whole ~40MB poisson3Db footprint in the L1: the ROI runs 99.7%
L1-hit — the only cold array is `y[]`, first touched inside the
kernel (10,704 write misses = exactly 685KiB/64B), at ~25ns each
under perfect memory (negligible: 0.27B ticks aggregate).

Result: **6.75M cycles = the exact compute floor** for this kernel on
this core (IPC 0.437; rename still blocks LQ-full 364K times even
all-hit — the 32-entry LQ is a floor-level constraint). Revised
ceiling accounting: max(3.44M DRAM-bandwidth floor, 6.75M) = 6.75M,
so vtyche@DDR4 (8.82M) sits at 76.6% of the ceiling — the earlier
"within ~9%" estimate from vtyche+perfect (8.06M) overstated the
floor by the cost of its residual 325K L1 misses and prefetch
machinery. Memory-side headroom above current vtyche is ~2.07M
cycles (up to 1.31x further).

Enabling this run required raising the snoop filters' capacity:
`SnoopFilter.max_capacity` defaults to 8MiB (`src/mem/XBar.py:149`)
and panics (`src/mem/snoop_filter.cc:197`) once a cache above the
xbar holds more than that. `rvv/unified_cache_hierarchy.py` now sets
`max_capacity="1GiB"` on the membus and per-core L2 buses; the cap
bounds tracking, not allocated storage, so normal runs are
unaffected.

## VTyche: self-clocked drain (2026-08-04; `drain_period` param removed 2026-08-12)

### What the drain did before

gem5 prefetchers are passive probe observers with no clock of their
own: `Queued::notify()` (`src/mem/cache/prefetch/queued.cc`) runs only
when the cache's probe system fires on an access, and VTyche's
conversion/emission engine (`drainEmission`) ran only inside
`calculatePrefetch` — i.e., only on demand-access notifies, at most
one buffered index line converted per event. `notifyFill` latched raw
payloads into the slice buffer but converted nothing. Consequence:
during core stalls (LQ-full — 1.09M rename blocks in the best run) the
drain froze exactly when the memory bus was idle, and bursty arrivals
left converted work stranded (`emissionLineLimited`, 135K events).
This demand-coupling is why vtyche's ~390K pfLate and its timeliness
residue were invariant between DDR4 and perfect-memory runs: every
pipeline stage and the demand cursor advanced on the same event clock.

### What this fork adds

An always-on every-cycle drain (originally a `drain_period` param,
Cycles default 1 with `0` restoring the legacy demand-clocked drain;
removed 2026-08-12 along with ReVeLA's — the Aug 5 sweep never used
another setting, and the legacy mode survives in the git history if
ever needed). VTyche self-schedules an `EventFunctionWrapper`
(`drainEvent`, `vector_tyche.cc`): while any producer holds buffered
or half-emitted conversion work, the drain
fires every cycle, converting (still at most one line
per firing — the serial lane-array cost model is unchanged) and
emitting into the queue. Re-arm points: `notify()` (demand event),
`notifyFill()` (new work), `getPacket()` (queue room freed by a cache
pull), and the tick itself.

Two deliberate bounds:

- **The stream walk stays demand-anchored** — `limitAddr` only
  advances in `streamAhead` on demand accesses, so self-clocking moves
  emission earlier *within* the sd window but can never run past it.
  The pollution exposure vs the legacy drain is only the stall-time
  backlog (≤ slice_buffer_entries lines per producer).
- **The tick waits for genuine queue room** (`room >= max(drain_floor,
  1)`) instead of forcing the floor. The floor exists because demand
  events are too scarce to waste; ticks are plentiful, so overshooting
  the queue and churning `pfRemovedFull` drops buys nothing. No-room
  ticks do not re-arm — the next pull or demand event does.

Mechanics of insertion outside `notify()`: `Queued::insert()` needs a
demand access's translation context (`pkt->req` VA/PA for same-page PA
arithmetic, ContextID for cross-page translation requests) plus a
`PrefetchInfo` template and the `CacheAccessor`. The overridden
`notify()` latches the most recent demand's `RequestPtr` (a
`shared_ptr` — safe to hold beyond the packet's life), a
`PrefetchInfo` copy (via the official `(pfi, addr)` constructor, which
nulls the data pointer), and the accessor pointer (same lifetime
pattern as `DeferredPacket::cache`). The tick wraps the saved request
in a transient stack `Packet` that `insert()` reads but never retains.
New stat: `selfDrainTicks` (firings that ran the emission engine).

## Feature: stream-demoting cache replacement (`--stream-demote`, 2026-08-05)

### What stock gem5 provides

Replacement policies (`src/mem/cache/replacement_policies/`) are
per-cache SimObjects; `BaseSetAssoc` passes the triggering packet to
`touch()` on every access (`base_set_assoc.hh:149`) and to `reset()`
at insertion (`:213`), and the policy base class declares pkt-aware
virtual overloads that default to the packet-less versions
(`replacement_policies/base.hh`). Stock policies ignore the packet
except `WeightedLRU`. Nothing in stock gem5 distinguishes single-use
streaming lines from reused lines except learned schemes (SHiP).

### What this fork adds

A demotion pipeline that uses the VTyche/GDP infrastructure as an
EXACT streaming oracle — a unit-stride vector load consumes a line's
whole spatial payload in one access, making its touch an
architecturally provable last use (no training, unlike SHiP):

1. `VectorChainTable` (`src/cpu/vector_chain_table.{hh,cc}`) grew a stream-page
   registry: `registerStreamPage(pa)` / `isStreamPage(pa)`, a 64-page
   FIFO+set of PHYSICAL pages, cleared on ROI reset. Page granularity
   sidesteps the VA/PA mismatch (caches are PIPT; prefetch requests
   carry no VA).
2. VTyche's `getPacket()` peek — the only point where a departing
   stream candidate's VA and PA are held together — now checks every
   VALID producer window (configured or not, so unlinked producers
   like spmv's a[] count) and publishes the candidate's physical page.
   Gather-target prefetches fall outside every producer window and are
   never registered: the discriminator is within-vector-traffic,
   stream vs gather, which scalar/vector split designs cannot express.
3. `StreamDemoteLRURP`
   (`replacement_policies/stream_demote_lru_rp.{hh,cc}`): LRU, except
   lines of registered stream pages are demoted to the LRU position
   (lastTouchTick=1 — older than any real touch, still a valid hit
   until evicted). `demote_on_insert=True` = L2 mode: demote at fill
   (the L2 copy is dead on arrival; the L1 copy serves the demand
   use). `demote_on_insert=False` = L1 mode: insert normally
   (the line owes its one use — insertion-demotion would evict the
   prefetcher's own lead), demote at first touch. Stats:
   demotedInserts/demotedTouches.
4. Run script: `--stream-demote {none,l1,l2,both}` (unified hierarchy
   + vector-chain-table prefetcher only) wires the policy in
   `rvv/unified_cache_hierarchy.py` sharing the per-core table
   instance; `--l2-prefetcher stride` + `--l2-pf-param` attach a
   second prefetcher to the unified L2 (requires --prefetcher-level
   l1) covering what an L1 vector prefetcher never learns (scalar
   ia[] walk, y[] stores).

### Results (poisson3Db spmv, unified, m32/40, vtyche sd16_sb8_dd64)

Control 8.813M cycles. `--stream-demote l2`: 8.690M (−1.4%), DRAM
reads 1.034M -> 743K (−28%, ~all of x[]'s capacity re-fetches),
queueing 45 -> 35ns, L2 demand hits +26%. `+ --l2-prefetcher stride`
(deg32/dist0): **8.410M = 1.785x vs matched base — best config to
date** (80.3% of the 6.75M compute-floor ceiling). `l1` alone: flat
(8.820M) — demotion cannot rescue a working set 20x the cache;
`both`: 8.788M, L1 demotion neutral on top of L2. The L2 is the
lever; the L1 mode exists for completeness.

### Demand-side registration (`--stream-demote-demand`, 2026-08-06)

The registry above is fed only by prefetch departures
(`VectorTyche::getPacket`), which couples the replacement policy to a
vector-chain-table prefetcher and limits classification to the producer index
arrays the prefetcher streams. To evaluate the policy in isolation —
and to cover demand-fetched lines and store streams — the registry
can now also be fed from the demand path:

1. `StaticInst::VecMemInfo` (`src/cpu/static_inst.hh`) grew a
   `UnitStrideStore` kind, implemented by a `vecMemInfo()` override
   on `VseMicroInst` (`src/arch/riscv/insts/vector.hh`) mirroring the
   existing `VleMicroInst` one. `VectorChainTable::dispatch` matches
   kinds exactly, so stores still take no part in chain construction.
2. `VectorChainTable` grew `demand_stream_pages` (Python param, default
   False) and `notifyDemandAccess(si, paddr)`
   (`src/cpu/vector_chain_table.{hh,cc}`): when enabled, a unit-stride vector
   load OR store registers its physical page.
3. The O3 LSQ calls it at translation finish —
   `LSQ::SingleDataRequest::finish` and `SplitDataRequest::finish`
   (`src/cpu/o3/lsq.cc`), the one demand-path point where the ISA
   identity and the translated PA are held together (the same
   VA/PA-meet argument as the prefetcher's getPacket hook). Split
   requests register every translated fragment's page; squashed
   translations are already filtered by the surrounding branch.
4. Run script: `--stream-demote-demand` sets the param on the
   existing per-core table (gdp/vtyche configs: both sources then
   feed the same registry) or, with a non-gdp prefetcher
   (`--prefetcher none`, `stride`, ...), creates a standalone
   `VectorChainTable` wired to `cpu.core.vector_chain_table` and the policy's
   `link_table` only (`rvv/vector_cache_hierarchy.py`,
   `rvv/unified_cache_hierarchy.py`) — chain construction runs idle
   in it; only the page registry is consumed.

Semantic difference from the prefetch-fed registry: DEMAND
registration marks every unit-stride-touched array (spmv: ia, ja,
a[], and the y[] store stream), not just prefetched producer index
arrays, and it works with any (or no) prefetcher — at the cost of the
first touched line per page inserting before its page is registered
(self-correcting one line later).

### Second-touch promotion (`--stream-demote-second-touch`, 2026-08-06)

The non-indexed RiVEC sweep (rivec_results/stream_replacement_*)
mapped the oracle's boundary: "unit-stride touch = last use" is
per-sweep truth, and iterative kernels that re-sweep the same arrays
regress on traffic (blackscholes 41x DRAM reads — 1.09M
demotedTouches, most of them HITS on victim-positioned lines whose
reuse evidence the unconditional demote discarded; jacobi-2d +44%;
somier +19%). The fix records the evidence instead of suppressing it:

1. `StreamDemoteLRU` gained a per-line CONSUMED bit
   (`SdReplData : LRUReplData`, own `instantiateEntry`): the FIRST
   touch of a stream-page line demotes and sets it (the
   architecturally known consumption, unchanged); a touch with the
   bit already set is a SECOND consumption of a line declared dead —
   observed cross-sweep reuse, recorded fact not prediction — and
   promotes via plain `LRU::touch` (new stat promotedTouches).
   `reset`/`invalidate` clear the bit: protection is per-residency
   (a re-fetched line must earn its second touch again), so the
   mechanism cannot over-protect.
2. True single-use streams never see a second touch, so every
   demotion win (spmv index streams, matmul's A, pathfinder's grid,
   axpy's insertion-placement effect) is preserved by construction.
3. Param `second_touch_promote` (ReplacementPolicies.py, default
   False so prior results stay reproducible); run script
   `--stream-demote-second-touch` wires it through both hierarchies.
   This is DIP's LIP-vs-LRU insight applied per-line with an exact
   trigger instead of set-dueling.

Measured result (second_touch sheet): incumbents get pinned (no-harm
exact on matmul/axpy/swaptions; pathfinder improves further; somier
partial, its cross-phase reuse re-touches fast enough) but
blackscholes did NOT recover (reads bit-identical despite 900K
promotions) — CHURN LOCKOUT: a re-fetched line re-inserts demoted, is
re-victimized before its second touch can ever arrive, so per-line
promotion gives evicted lines no re-entry path.

### Page promotion (`--stream-demote-page-promote`, 2026-08-06)

The response to churn lockout moves from line to page granularity:

1. `VectorChainTable::promoteStreamPage(pa)` removes the page from the
   stream registry AND records it in a promoted-page FIFO+set
   (`promoted_page_entries`, default 1024 ≈ 4KB) that
   `registerStreamPage` consults — without the block, the demand path
   re-registers the page on its very next unit-stride access and
   unlearning is instantly undone. Cleared at ROI resets with the
   rest of the table.
2. `StreamDemoteLRU`: when `page_promote` is set, the second-touch
   event (same consumed-bit detector) additionally calls
   `promoteStreamPage` (stat pagesPromoted). After promotion the
   page's fills insert as plain LRU — the churn set's re-entry path —
   and its still-demoted resident lines promote naturally at their
   next touch (isStream is now false).
3. The consumed bit's role reduces to pure reuse DETECTION (the
   response is page-level); it stays because in demote-on-insert mode
   a line's first legitimate use is otherwise indistinguishable from
   a re-reference — firing unlearning on first touch would
   self-destruct the policy on true streams within one sweep.

### Monotone-arm gate (`--stream-demote-monotone`, 2026-08-10)

The touch-evidence fixes above are all REACTIVE — they pay demotion
damage before they can learn (and blackscholes's write-stream pages
never generate the evidence at all: every pass's touch is a fresh
residency's first touch, the churn lockout). The monotone-arm gate is
PROACTIVE, built on one observation from the RiVec sources: every
harmful case of demoted-line reuse in the suite comes from an
address-identical re-sweep (blackscholes's `NUM_RUNS` wrapper,
jacobi's `kernel(A,B); kernel(B,A)` ping-pong, somier's per-timestep
phase pipeline) — and a re-sweep is directly observable as a demand
access BELOW the stream's high-water mark.

#### What the existing method did

`notifyDemandAccess` registered every unit-stride vector load/store's
page unconditionally (when `demand_stream_pages` is set), asserting
"unit-stride ⇒ single-use" — exact within one sweep, blind across
sweeps.

#### What this fork adds

1. `VectorChainTable` grows a **Stream Direction Table** (16 entries,
   PC-keyed, LRU-victimized; `src/cpu/vector_chain_table.hh`): per stream
   `startAddr/minAddr/maxAddr` (VA), a direction latch, and
   `armed`/`revoked` bits. Tracking is in VIRTUAL address space — a
   VA-contiguous sweep is not PA-contiguous across page frames, so a
   PA-space high-water mark would false-revoke at frame boundaries.
   The VA rides along from the LSQ call sites
   (`src/cpu/o3/lsq.cc`, Single + Split `finish()`), which also now
   pass the PC.
2. **Arming**: a stream registers pages only after covering
   `arm_distance` (default 4096 B) of VA monotonically — the literal
   "enough distinct stream lines", since distance = distinct lines x
   64 for a unit-stride sweep, and VLEN-independent because it is a
   distance, not an access count.
3. **Sticky revocation**: an access more than `backward_slack`
   (default 1024 B, absorbs OoO completion jitter) below the
   high-water mark (above, for descending streams) proves the sweep
   recurs; the stream's lines have a future, so demoting them trades
   hits for DRAM refetches. The entry disarms permanently (until the
   ROI stats-reset wipe) AND its already-registered pages are dropped
   from the registry — the page FIFO entries now carry the
   registering PC (`StreamPageRec`) to make that walk exact. Without
   the retroactive drop, pages registered before the re-sweep was
   detected keep demoting fills for a whole further pass (the jacobi
   timestep-2 trap).
4. Scope: only DEMAND-side registrations are gated; prefetcher-side
   `registerStreamPage` calls (vtyche/gdp `getPacket`) stay untagged
   (pcTag 0) and ungated — the run script therefore requires
   `--stream-demote-demand` with `--stream-demote-monotone`.
   `StreamDemoteLRURP` is untouched: all the new intelligence is in
   what enters and stays in the registry.
5. Stats (`vector_chain_table` group): `monotoneStreamsArmed`,
   `monotoneStreamsRevoked`, `monotonePagesDropped`,
   `monotoneRegsSuppressed`. Direction-table state clears at ROI
   resets only, NOT on DCT capacity clears (`clearAll`) — a lost
   revocation would cost a whole re-sweep of demotion damage to
   re-learn, while the page registry re-fills from the demand path
   within a few accesses.

Predicted (to be validated against
`rivec_results/stream_replacement_data/`): blackscholes/jacobi/somier
revert to ≈ base (revoked in ≤ 1 pass); axpy/matmul keep their wins
(monotone single sweeps never revoke); pathfinder reverts to ≈ base
(its win depended on demoting a recurring sweep — recoverable later
via a footprint-conditional BIP fallback on revoked streams); spmv
keeps the full demotion win (`streamsRevoked == 0`, single-sweep ROI,
`_spmv/src/main.c:153-155`).

## VTyche: DRAM row-aware issue order (`row_schedule_bits`, 2026-08-07)

### What stock gem5 does

`Queued::getPacket()` (`src/mem/cache/prefetch/queued.cc:239-264`) pops
`pfq.front()` unconditionally. `addToQueue` (same file, 487-530) keeps
the queue ordered by `DeferredPacket::operator>`, which compares
**priority only** (`queued.hh:95-106`) — within one priority level the
queue is strict FIFO. No gem5 prefetcher considers the DRAM-level
consequences of issue order; the only reordering in the memory path
happens later, in the controller's FR-FCFS scheduler
(`DRAMInterface::chooseNextFRFCFS`, `src/mem/dram_interface.cc:60`),
which can only reorder requests already resident in its 64-entry read
queue.

### The gap this targets

Two queued targets in the same DRAM row can be separated by enough
intervening issues that the row closes between them. The second then
pays a full `tRP + tRCD` (28.3 ns on `DDR4_2400_8x8`) instead of
riding the open row for `tCL` alone. FR-FCFS cannot fix this: it only
sees requests that are simultaneously outstanding, and in these runs
the read queue holds 6.63 entries on average (`avgRdQLen`) against a
capacity of 64, because concurrency is MSHR-limited well below
saturation.

### What this fork adds

`VectorTychePrefetcher.row_schedule_bits` (Unsigned, default `0` =
disabled/FIFO). When non-zero, `VectorTyche::rowSchedule()` runs at
the top of the overridden `getPacket()` and promotes the oldest queued
prefetch whose row (`paddr >> row_schedule_bits`) matches the last
issued prefetch to the head of its priority group, via an O(1)
`std::list::splice`.

Set it to **13** for `DDR4_2400_8x8`: the row buffer is
`devices_per_rank(8) x device_rowbuffer_size(1KiB)` = 8 KiB
(`mem_interface.cc:62`), and under the default `RoRaBaCoCh` mapping
with one non-interleaved channel `decodePacket`
(`dram_interface.cc:856-872`) places column bits at `paddr[12:6]`,
bank at `[16:13]`, rank at `[17]`. So one DRAM row is exactly an
8 KiB-aligned physical region and `paddr >> 13` identifies
row+bank+rank in a single shift — no address-map plumbing needed.

Three invariants keep the reordering safe:

- **Equal priority only.** The scan stops at the first entry whose
  priority differs from the head's, so a lower-priority packet is
  never hoisted over a higher-priority one and `addToQueue`'s
  insertion scan stays correct.
- **Due entries only.** Both the head and the candidate must have
  `tick <= curTick()`. Every reordered request could have issued this
  cycle anyway, so nothing is delayed past its ready time and
  `nextPrefetchReadyTime()` (`pfq.front().tick`) still reports a due
  tick.
- **Reorder before the capture peek.** `getPacket()`'s range-based
  IRT capture inspects `pfq.front()`; `rowSchedule()` runs first so
  that peek sees the entry actually about to be popped.

New stats: `rowPromotions` (a row-mate was found and promoted) and
`rowScheduleMisses` (the queue held no due row-mate).

### Usage

```bash
--prefetcher vtyche --pf-param row_schedule_bits=13
```

### Measurement: poisson3Db spmv, unified hierarchy (2026-08-07)

Controlled A/B on the best unified vtyche config (`sd16 sb8 dd64`,
`--unified-cache --prefetcher-level l1 --l1d-mshrs 16 --vlen 2048`),
both runs on the same binary, differing **only** in
`row_schedule_bits`. Both verified (`Verification pass`).

| Stat | `bits=0` (FIFO) | `bits=13` | Δ |
|---|---|---|---|
| `numCycles` | 9,525,971 | 9,677,667 | **+1.59% (worse)** |
| `dram.readRowHitRate` | 73.77% | 74.33% | +0.56 pp |
| `bytesPerActivate::mean` | 246.29 B | 251.64 B | +2.2% |
| row activations | 273,612 | 267,821 | **−2.1%** |
| `dram.avgQLat` | 33.03 ns | 33.74 ns | +2.2% |
| `dram.avgMemAccLat` | 50.52 ns | 51.23 ns | +1.4% |
| `pfLate` | 328,564 | 340,314 | +3.6% |
| `pfRemovedFull` | 34,474 | 66,904 | **+94%** |
| `demandMshrMisses` | 337,335 | 357,371 | +5.9% |
| `dram.readBursts` | 1,034,189 | 1,034,274 | +0.01% |
| `rowPromotions` | 0 | 959,710 | — |
| `rowScheduleMisses` | 0 | 1,199,681 | — |

**The mechanism works and does not pay.** A row-mate was found on
44.4% of issue decisions (959,710 of 2,159,391), and the DRAM side
responded exactly as designed: 2.1% fewer activations, +0.56 pp row
hit rate, +2.2% bytes per activation. The program still ran 1.59%
slower.

Two costs swamp the saving:

1. **Timeliness (dominant, directly measured).** Deferring a prefetch
   behind a row-mate keeps it queued longer, so more entries are
   evicted when the queue fills — `pfRemovedFull` nearly doubled — and
   more of the survivors land late (`pfLate` +3.6%). Net effect:
   `demandMshrMisses` rose 5.9%, i.e. ~20K more demand misses went
   uncovered. The 2.1% activation saving does not buy that back in a
   workload whose critical path is the index→data round trip.
2. **Bank-group serialization (secondary).** Same row implies same
   bank implies same bank group, so consecutive column commands to a
   clustered row are spaced by `tCCD_L` (5 ns) instead of the
   `tBURST` (3.332 ns) available when bursts alternate bank groups
   (`dram_interface.cc:442-460`). This is visible as `avgQLat` rising
   2.2% *despite* 2.1% fewer activations — the controller issued fewer
   ACTs but took longer to get the column commands out.

This is consistent with the prior characterization that the workload
is round-trip-latency-bound, not DRAM-throughput-bound: read bus
utilization is only ~36%, so activate overhead was never the binding
constraint, and spending prefetch timeliness to reduce it trades the
scarce resource for the abundant one.

**Recommendation: leave `row_schedule_bits=0` (the default).** The
mechanism is retained because it is cheap, off by default, and gives a
clean way to re-test the hypothesis in regimes where activate rate
*does* bind — high bus utilization (multi-core, wider VLEN, higher
degree) or matrices with real per-row clustering (venkat25, lhr07,
Na5, whose banded structure gives more than the ~1.25 target lines per
row poisson3Db offers).

### Bounded reorder window (`reorder_window_size`, 2026-08-12)

As introduced above, `rowSchedule()` scanned the head's **entire**
priority group for a row-mate — effectively a CAM over the whole
prefetch queue (up to `queue_size`, default 32 entries in
`Prefetcher.py`'s `QueuedPrefetcher` base). That is both generous as
hardware and, per the measurement above, part of why the mechanism
hurt timeliness: an entry promoted from deep in the queue jumps over
every older entry in between, and the 2026-08-07 A/B showed the
resulting queue-residence growth (`pfRemovedFull` +94%, `pfLate`
+3.6%) swamping the activation saving.

`VectorTychePrefetcher.reorder_window_size` (Unsigned, default `0` =
unbounded, the pre-existing behavior) bounds the scan in
`VectorTyche::rowSchedule()` (`src/mem/cache/prefetch/vector_tyche.cc`)
to the first N prefetch-queue entries, counted from and including the
head. A promotion can then only hoist an entry over at most N−1
others, capping the age-order perturbation, and the model corresponds
to a fixed-depth comparator window at the queue head rather than a
full-queue CAM. Two details:

- Window slots are **positional**: an in-window entry that fails the
  existing due/priority checks still consumes a slot, exactly as a
  hardware comparator wired to that queue position would.
- `reorder_window_size=1` leaves only the head in the window, so no
  reordering can occur — equivalent to FIFO, but with
  `row_schedule_bits` still spending `rowScheduleMisses` bookkeeping.
  The prior full-queue behavior remains available via the default `0`.

All three safety invariants of the original mechanism (equal priority,
due entries only, reorder-before-capture-peek) are unchanged; the
window is a strictly tighter candidate filter inside the same scan.

```bash
--prefetcher vtyche --pf-param row_schedule_bits=13 \
    --pf-param reorder_window_size=8
```

## Feature: vector_indirect_eval — synthetic gather-locality benchmark (2026-08-11)

### What gem5 provides

Nothing benchmark-side: gem5 has no synthetic workload infrastructure.
The benchmark instead leans on two documented-nowhere properties of the
simulator that we verified in source:

1. **SE-mode physical frame assignment.** Every path funnels through
   `Process::allocateMem` (`src/sim/process.cc:318-345`), which draws
   frames from a `FreeList<Addr>` pool (`src/sim/mem_pool.hh:57`) that
   carves best-fit ranges from their low end
   (`src/base/free_list.hh:146-175`). At program load the pool has never
   seen a free, so it degenerates to a bump allocator. `.bss` segments
   are memset through the Always-allocating proxy at load time
   (`src/base/loader/memory_image.cc:44-47`,
   `src/mem/se_translating_port_proxy.cc:60-63`) — ascending VA order,
   zero simulated cycles — so a static array gets contiguous ascending
   frames: **PA = VA + c** across the whole array. Heap/mmap memory is
   instead allocated one page per first touch
   (`src/sim/mem_state.cc:387-396`), i.e. in access order — which is why
   the benchmark's arrays must be static, not malloc'd.

2. **DRAM address decode.** `DRAMInterface::decodePacket`
   (`src/mem/dram_interface.cc:834-918`) splits a physical address, for
   the default `SingleChannelDDR4_2400` (= DDR4_2400_8x8,
   `src/python/gem5/components/memory/single_channel.py:63`; 8 KiB row,
   16 banks, 2 ranks, 1 channel, `src/mem/DRAMInterface.py:618`), into:
   byte-in-line 5:0, column 12:6, bank 16:13, rank 17, row 18+.
   Consequences used by the benchmark: +8 KiB = next bank; **+256 KiB =
   same bank, next row** (the conflict stride); a 4 KiB page always lies
   inside one 8 KiB row, so "same virtual page ⇒ same frame ⇒ same DRAM
   row" holds regardless of the load-time constant c (which is only
   4 KiB-aligned — full-8 KiB-row experiments would be a coin flip).

### What we added

`rvv/vector_indirect_eval.c` (built by `rvv/Makefile.tests`, linked
against `util/m5/build/riscv/out/libm5.a`, ROI markers follow RiVec
`common/roi.h`): a single-pass `sum += A[B[i]]` gather kernel (all-e64:
vle64 → vsll 3 → vluxei64 → vadd, one vredsum — avoiding the latent
vle32-under-e64m2 widening-chain corruption) over a generator-built
index array with exactly controllable locality:

    vector_indirect_eval <mode> <N> <L> <pool_bytes> [W] [seed]

B is built in blocks of L entries targeting one randomly drawn unit —
offered same-unit locality is exactly (L−1)/L:

- `line_locality`: unit = 64 B cache line (elements walk k mod 8).
  Optional W interleaves tiles of W blocks round-robin so same-line
  reuses land exactly W apart (stack distance W−1); W is line-mode-only
  because DRAM rows open/close — interleaving would destroy
  arrival-order row locality and conflate the knob with scheduler
  reordering.
- `row_locality`: unit = 4 KiB page-block (L ≤ 64 distinct lines in one
  seeded scrambled order; line locality pinned at 0%).
- `bank_conflicts`: unit = the first 4 KiB page of same-bank rows at
  256 KiB stride, consecutive-repeat rejected. Only the row's first
  page is used: the second page sits in a different bank whenever c is
  an odd number of pages. Requires `pool ≥ 32·N·64` (same-bank rows are
  1/32 of the address space). The only mode that depends on PA = VA + c.

Every touched element is filled `A[j] = j` during generation (layout is
already fixed by the load-time allocation, so no sequential fill pass is
needed), making the correct ROI sum equal the sum of B — checked after
the ROI (`RESULT: PASS/FAIL`). Before the ROI an independent structural
pass over B re-verifies every parameter-promised property (block unity,
per-block distinct lines, bank stride, lag-W spacing, exact distinct
line/page recount) and prints machine-parseable `KEY: value` lines —
`DISTINCT_LINES` is the exact compulsory-miss floor (repeat draws are
allowed and counted, not prevented).

Sizing defaults (pool ≥ ~100× vector L2 keeps accidental residency
<1%): line_locality N=1M pool=256 MiB; row_locality N=256K pool=256 MiB;
bank_conflicts N=128K pool=512 MiB. Run with the prefetcher MMU
translation opt-in and `prefetch_on_pf_hit=false`.

Validated 2026-08-11: 134-assertion host-native matrix (all modes ×
L × W sweeps, determinism, all rejection paths) plus baseline gem5 runs
checked by `rvv/check_vector_indirect_eval.py`:

- line_locality N=64K L=4: dram.readBursts within 0.5% of the
  DISTINCT_LINES + N/8 floor.
- row_locality N=256K L=8: measured DRAM read row-hit rate 87.9% vs
  88.8% expected (offered 87.5% composited with the B stream);
  readBursts within 0.2% of N + N/8.
- bank_conflicts N=128K L=1: row-hit rate 10.1% vs 11.0% expected
  (A-side offered 0%); 47.8 ROI cycles/access vs row_locality's 10.0 —
  the precharge/activate serialization the mode exists to create.

Two stat-accounting facts the checker encodes: (1) L1 `demandMisses`
reads ~N in line mode by design — MSHR merges count as misses — so
`dram.readBursts` is the floor metric; (2) in the row modes every
access reaches DRAM (expected reads = N + N/8): the N − DISTINCT
revisits are long-distance and get evicted, and in bank_conflicts this
is structural — the 256 KiB stride is a multiple of both caches'
way spans, so every target aliases into a single cache set and
revisits are never cache-absorbed.

## Feature: ReVeLA prefetcher (`--prefetcher revela`, 2026-08-11)

Implementation of "Exploiting Vector Code Semantics for Efficient Data
Cache Prefetching" (Martinez Palau, Torrents, Armejach, Casas —
ICS'24), the Register Vector Length Agnostic (ReVeLA) prefetcher, on
this fork's ARA O3 model. See `revela.pdf` in the repo root.

### The idea

Vector Length Agnostic code strip-mines loops as
`vl = vsetvl(n - i)`: the AVL passed in (the paper's Requested Vector
Length, RVL) is the element count the loop still has to process, and
the granted `vl` (GVL) is what one instruction can cover. A
unit-stride access at `access@` therefore *announces* that
`[access@ + GVL, access@ + RVL)` will be accessed by the following
iterations — prefetch candidates that are certain, not predicted.
Two properties fall out: near-perfect accuracy (only announced lines
are requested) and a trigger decoupled from cache events (the paper
evaluates its structures every cycle and issues whenever the memory
system has headroom). Coverage is limited to announced unit-stride
streams, so the paper positions ReVeLA as a *complement* to a
conventional coverage prefetcher, not a replacement.

### What stock gem5 provides

- No notion of the vsetvl AVL after execution: `vl = min(AVL, VLMAX)`
  is architectural state, the AVL itself is discarded. gem5's RISC-V
  decoder embeds the *granted* `vl` and `vtype` in every vector
  instruction's `ExtMachInst` (`src/arch/riscv/types.hh`, bitfields
  57:41 and 39:32) because decode depends on them — so GVL is
  recoverable from any vector memory micro-op, RVL is not.
- Prefetchers (`src/mem/cache/prefetch/base.cc`, `queued.cc`) are
  event-driven: `notify()` fires on cache accesses/misses and
  `calculatePrefetch()` returns candidate addresses. There is no
  every-cycle trigger path. The cache pulls queued prefetches only
  when `mshrQueue.canPrefetch()` holds (`src/mem/cache/base.cc`),
  which reserves `demand_mshr_reserve` MSHRs for demand traffic —
  the mechanism the paper's ">= 8 free MSHRs" issue gate maps onto.

### What this fork already had that ReVeLA reuses

- The CPU-to-prefetcher sideband pattern: a per-core SimObject wired
  to both `BaseCPU.<x>_table` and the prefetcher (GDP's
  `VectorChainTable`, `src/cpu/vector_chain_table.hh`).
- Issue-stage operand snooping in `src/cpu/o3/inst_queue.cc` (GDP
  reads gather bases / .vx scalars off the operand read issue is
  already doing) and the raw-encoding classification technique
  (`StaticInst::getEMI()`, no ISA edits).
- The LSQ translation-finish hook in `src/cpu/o3/lsq.cc` (GDP's
  `notifyDemandAccess`), where a micro-op's ISA identity and VA meet.
- `StaticInst::vecMemInfo()` (`src/cpu/static_inst.hh`), the
  ISA-neutral classification virtual overridden by
  `VleMicroInst`/`VseMicroInst` (`src/arch/riscv/insts/vector.hh`).
- The self-clocked drain machinery from VTyche2
  (`src/mem/cache/prefetch/vector_tyche2.cc`): an
  `EventFunctionWrapper` that inserts prefetches outside notify(), a
  latched translation context (Request + PrefetchInfo +
  CacheAccessor from the last observed access), and VA-mode
  prefetching with automatic MMU registration.

### What this fork adds

**1. RVL capture — `src/cpu/o3/inst_queue.cc` (issue stage).**
`vset{i}vl{i}` is identified from the raw encoding (OP-V major opcode
0x57, funct3 0b111 — `getEMI()`, no ISA edits) and its AVL snooped at
issue: `vsetivli` takes the uimm from the rs1 field; `vsetvli`/
`vsetvl` with rs1!=x0 read the rs1 register operand (sources are
ready at issue, same as GDP's snoop); rs1=x0/rd!=x0 requests VLMAX —
the code reveals no stream end, so the shadow is *invalidated*
rather than guessed; rs1=x0/rd=x0 keeps vl and the shadow. The paper
propagates RVL/GVL with every memory request; a single per-core
shadow register models that exactly for strip-mined loops, where
every iteration re-executes vsetvl before its loads/stores (stale
shadows are additionally caught by the `AVL < vl` guard below).

**2. GVL exposure — `src/cpu/static_inst.hh`,
`src/arch/riscv/insts/vector.hh`.** `VecMemInfo` gains `microIdx`
and `vl` fields, filled by the existing `VleMicroInst`/`VseMicroInst`
overrides from the class's `microIdx` and `machInst.vl`. The STT
update runs once per macro-op (micro-op 0 carries the base address)
with the whole group's extent `vl * (EEW/8)` bytes. Indexed,
strided, segment and whole-register ops keep `kind = None` and are
ignored — they don't form VA-contiguous streams.

**3. `RevelaStreamTable` — `src/cpu/RevelaTable.py`,
`src/cpu/revela_table.{hh,cc}` (new files).** The paper's Stream
Tracking Table as a per-core sideband SimObject
(`BaseCPU.revela_table` + `RevelaPrefetcher.stream_table`, wired by
the hierarchy exactly like the vector chain table; `src/cpu/base.{hh,cc}`,
`src/cpu/BaseCPU.py` grew the parallel param). The O3 LSQ calls
`notifyVectorAccess(si, vaddr)` at translation finish
(`src/cpu/o3/lsq.cc`, both Single and Split requests — Split uses the
unsplit `_mainReq` VA, one update per micro-op, not per fragment).
Entries are `{current@, limit@, prefetched-up-to, valid, 8-bit LRU}`
keyed by the loop-invariant limit address `access@ + RVL_bytes`
(paper Section 3.2): forward hits advance
`current@ <- access@ + GVL_bytes` and reset the LRU (aging all
others; saturation at `lru_max(255)` invalidates stale streams);
backward hits are ignored (OoO reordering); `current@ >= limit@`
invalidates (final access); misses allocate over an invalid entry or
the largest-LRU victim. Two deliberate deviations from the paper's
flowchart, both documented in the header: `prefetched_distance` is
stored as an *address* (`prefetchedUpTo`) rather than a line count —
identical information, but the CPU side doesn't know the cache line
size — and allocation is skipped when `RVL <= GVL` (nothing left to
prefetch; a dead entry would only depress the aggressivity). All
state wipes at every stats reset (ROI convention, 2026-07-13).

**4. `RevelaPrefetcher` — `src/mem/cache/prefetch/revela.{hh,cc}`,
`Prefetcher.py` (new class).** The cache-side trigger logic (paper
Section 3.3) on VTyche2's self-clocked drain skeleton: the drain
fires every cycle while work is pending (fixed — the paper's cadence;
the `drain_period` params on ReVeLA, VTyche and VTyche2 were all
removed 2026-08-12: any other setting is a different design); each
evaluation checks free queue room (`queue_size(16)`, the paper's
prefetch queue), computes the aggressivity cap from the live-stream
count — `max_prefetch_distance(64)` lines at <2 valid entries, /2 at
<4, /4 at <8, /8 otherwise (the paper's 64/32/16/8 table) — and lets
only the streams at the *minimum* prefetched distance emit
`degree(1)` lines each, advancing `prefetchedUpTo`. Emissions from a
demand-access notify use the standard `calculatePrefetch()` path;
drain-tick emissions replicate `Queued::notify`'s insert loop with
the latched translation context (the VTyche2 pattern). Addresses are
virtual (paper Section 3.5: streams are VA-contiguous, PAs break at
page frames) and translate through the registered MMU, so ReVeLA
also prefetches TLB entries, as in the paper. MSHR throttling is the
cache's own `canPrefetch()`/`demand_mshr_reserve` — set the hosting
cache's reserve to 8 to reproduce the paper's gate exactly.

**5. Wiring — `rvv/prefetcher_factory.py`,
`rvv/riscv-rvv-se-ara-prefetcher.py`,
`rvv/{vector_cache,unified_cache}_hierarchy.py`.**
`--prefetcher revela` (vector side only in the split hierarchy — the
guard mirrors gdp's), `--revela-stt-entries(16)` sizes the table
(the paper's sensitivity axis; 4 entries already give 91% of the
benefit), everything else via `--pf-param` (see MEMORY_CONFIG.md).
The hierarchies create one `RevelaStreamTable` per core and hand it
to both the core and the prefetcher, following the vector-chain-table pattern
verbatim.

### Invocation

```bash
build/RISCV/gem5.opt rvv/riscv-rvv-se-ara-prefetcher.py \
    --enable-chaining --vlen 512 --vector-timing-throughput 4 \
    --simd-units 2 \
    --prefetcher revela --prefetcher-side vector --prefetcher-level l1 \
    /path/to/riscv-binary

# Paper-style sweeps:
#   aggressivity: --pf-param max_prefetch_distance={8,16,32,64,128}
#   STT size:     --revela-stt-entries={1,2,4,8,16,32}
```

`--prefetcher-level l1` is the closest analog of the paper's
placement: their vector unit interfaces directly to the (single) LLC,
and in the split hierarchy the vector L1D is the first cache the
vector unit talks to. `l2` (the script default) works — the STT is
fed from the LSQ either way — but at L2 the drain's re-arm and
translation-context refresh only see L1-miss traffic.

### Caveats

- The RVL shadow is issue-order state updated speculatively:
  wrong-path vsetvls can overwrite it. Like GDP's issue-time snoops
  this is self-healing (the next good-path vsetvl re-writes it), and
  the `AVL < vl` guard drops obviously-stale pairings; a genuinely
  wrong-but-plausible AVL costs one junk STT entry, aged out by LRU.
- Only unit-stride, non-segment vector loads/stores form streams.
  Whole-register loads carry no AVL semantics; strided/indexed
  accesses are not VA-contiguous (the paper ignores them too).
- Stat visibility: the CPU-side table reports under
  `system...revela_table` (`streamsAllocated`, `sttUpdates`,
  `streamsCompleted`, `vsetvlSnooped`, ...); the prefetcher under the
  hosting cache (`linesEmitted`, `selfDrainTicks`,
  `aggressivityThrottled`, plus all standard `Queued` stats —
  `pfIssued`, `pfUseful`, `pfUnused` give the paper's
  useful/late/useless split together with `pfLate`).

### Fix: cross-page translation kick for drain-inserted prefetches (2026-08-11)

The first validation run (RiVec axpy, 256K doubles, `--prefetcher
revela --prefetcher-level l1`) was performance-flat vs baseline with
only 356 lines emitted against ~98K stream lines and `drainNoRoom` at
1.78M: the prefetch queue wedged. Root cause, in stock gem5
(`src/mem/cache/prefetch/queued.cc`): translations for cross-page
candidates parked in `pfqMissingTranslation` are started ONLY by
`getPacket()` — and the cache only calls `getPacket()` when
`nextPrefetchReadyTime()` is finite, which requires a non-empty
`pfq`. A cycle-driven prefetcher whose lookahead runs 1-2KiB ahead
emits mostly cross-page candidates, so `pfq` drains to empty while
all 16 queue slots sit untranslated → `MaxTick` → no more pulls → no
translations → deadlock, broken only by rare port-idle fall-through
pulls (`BaseCache::getNextQueueEntry`). Event-driven prefetchers
dodge this by inserting during `notify()` with mostly near-page
candidates, keeping `pfq` non-empty.

Fix: `Queued::processMissingTranslations` moved from private to
protected (`queued.hh`, access-only change, upstream behavior
untouched), and `ReVeLA::drainTick`/`notify` kick it after inserting
(`revela.cc`) — exactly the call `getPacket()` would have made if the
cache were still pulling.

### Validation (RiVec axpy, 2026-08-11)

axpy 256K doubles (2 MiB/array vs 512 KiB vector L2), VLEN 512,
4-lane timing, split hierarchy, revela on the vector L1D, all other
knobs default. Baseline = same config, no prefetcher:

- Correctness: `Result ok !!!` in both runs.
- ROI cycles: 1,643,310 -> 695,451 = **2.36x speed-up** (paper
  reports 2.25x for axpy at 256 GB/s, 2.73x at 512 GB/s).
- Coverage: L1D demand MSHR misses 73,468 -> 7,932 (89% covered);
  pfUseful 53,239 + pfLate 30,693 of 96,229 issued.
- Accuracy: ~2% of issued prefetches unaccounted (mostly in-flight at
  ROI end) — consistent with the paper's ~1% useless rate. 10,237
  redundant issues were dropped on the cache's tag check
  (`pfHitInCache`), an overlap artifact of the shadow-race entries
  below.
- Capture: `vsetvlSnooped` = 32,768 = exactly n/vl iterations;
  STT accounting exact (updates + backward + allocs = 3 unit-stride
  accesses x 32,768 iterations).

Known wart (harmless here, worth watching): the RVL shadow is updated
at ISSUE, and gem5 does not serialize `vsetvli` when vl/vtype are
unchanged, so a next-iteration `vsetvli` can issue before the current
iteration's loads translate. The mismatched shadow makes those loads
STT-miss and allocate junk entries with slightly-short limits (80 of
98,304 accesses in the pre-fix run; 6 in the fixed run). Junk entries
self-heal (LRU aging, `staleInvalidations`) and their emissions cover
the same arrays, but they inflate the valid-entry count (lower
aggressivity cap) and can duplicate emissions (`pfBufferHit`,
`pfHitInCache`). If it ever matters on other kernels, the fix is
per-request RVL propagation (the paper's model) instead of the
one-register shadow.

## Feature: VHybrid prefetcher (`--prefetcher vhybrid`, 2026-08-12)

**Existing mechanisms.** Two of this fork's prefetchers each solve half
of the vector-gather problem: ReVeLA (`--prefetcher revela`,
`src/mem/cache/prefetch/revela.{hh,cc}`) prefetches unit-stride streams
whose full extents the program ANNOUNCES through the vsetvl AVL
(CPU-side `RevelaStreamTable`, `src/cpu/revela_table.hh`), with the
paper's aggressivity table and an every-cycle self-clocked trigger —
but it cannot touch gathered data. VTyche (`--prefetcher vtyche`,
`src/mem/cache/prefetch/vector_tyche.{hh,cc}`) prefetches gather
targets by capturing index-array lines and converting them through the
collapsed chain `target = base + (extend(idx) << shift)`
(`VectorChainTable`, `src/cpu/vector_chain_table.hh`) — but its index
stream walk is demand-anchored (`limitAddr` = demand + sd), which
bounds index lead and therefore target timeliness.

**What this fork adds.** `VHybrid`
(`src/mem/cache/prefetch/vhybrid.{hh,cc}`, `VHybridPrefetcher` in
`Prefetcher.py`, debug flag `VHybrid`) composes the two: ReVeLA's
stream engine (STT + aggressivity + min-distance fairness + every-cycle
drain, `revela.cc` logic reused structurally unchanged) REPLACES
VTyche's stream walk, so index lookahead is licensed by the announced
extent (up to `max_prefetch_distance` = 64 lines) instead of demand
anchoring; VTyche's capture/convert half (IRT range capture at
`getPacket`, fill payload latch, one-line-per-event lane-array
conversion, cross-line dedup, `vector_tyche.cc` logic reused
structurally unchanged) rides on those streams.

The join: `RevelaStreamTable::SttEntry` gained one additive field,
`lastPc` — the PC of the access that last updated the entry, stamped
by the existing LSQ report path (`notifyVectorAccess`, which gained a
`pc` argument; call sites `src/cpu/o3/lsq.cc`). A stream whose
`lastPc` the chain table reports as a linked producer
(`producerInfo()`) is an INDEX stream: its emitted lines register for
fill capture and publish their physical pages for `--stream-demote`.
Producer status is re-evaluated on use, never latched in the STT, so
late-forming links and wholesale chain-table clears self-heal. The
staleness abort uses the announced stream's demand cursor
(`currentAddr` of the producer's live STT entry) where VTyche used its
walk cursor.

Per-tick emission order is targets-first, then the stream round with
the remaining room; both count against one combined
`pfq + pfqMissingTranslation` budget (`queue_size` = 64, VTyche's
depth, since one queue now carries stream lines and target bursts).
The drain keeps ReVeLA's `processMissingTranslations` kick (both
traffic classes are heavily cross-page).

**Wiring.** First prefetcher needing BOTH sideband tables:
`stream_table` (+ CPU `revela_table`) and `link_table` (+ CPU
`vector_chain_table`). The hierarchies' independent `_params` guards
(`rvv/vector_cache_hierarchy.py`, `rvv/unified_cache_hierarchy.py`)
wire both automatically; `rvv/prefetcher_factory.py` lists vhybrid in
`VA_PREFETCHERS`, `VECTOR_CHAIN_TABLE_PREFETCHERS` and
`REVELA_TABLE_PREFETCHERS`.

**Deliberately absent: announcement stitching.** Streams announced
per-strip (nested-loop CSR spmv: per-row vsetvl) keep their tiny
extents and the STT's RVL<=GVL allocation skip. That shape is out of
coverage BY DESIGN — the JDS spmv variant
(`riscv-vectorized-benchmark-suite/_spmv`, `jds` CLI arg) is the
sparse evaluation vehicle, matching how the ReVeLA paper's own spmv
must be formulated (per its Figure 6 logic, per-row announcements on
their 2KB-register machine would emit nothing, yet they report 1.37x).

### SPMV flat (nonzero-parallel) kernel variant (`flat` CLI arg, 2026-08-16)

**The existing method.** The RiVec spmv kernel
(`riscv-vectorized-benchmark-suite/_spmv/src/spmv.c`) vectorizes
per row: each row issues `vsetvl(avl = row nnz)`, so the announced
extents are tiny fragments (poisson3Db: ~86K announcements averaging
~4 elements) and stream boundaries recur every few elements. The
`jds` variant stretches extents to jagged-diagonal length but still
announces per diagonal.

**What this adds.** A third format literal, `flat`
(`_spmv/src/main.c`, same any-order CLI convention as `jds`),
selecting a nonzero-parallel kernel
(`spmv_intrinsics_flat`/`spmv_serial_flat`, `_spmv/src/spmv.c`) with
Blelloch-style phase separation (Blelloch/Heroux/Zagha 1993): phase 1
is ONE strip-mined loop over all nnz (value load, `ja` load, gather,
multiply, store to a product array `t[nnz]`), so its first vsetvl
announces the FULL nnz extent — the true end of contiguity — and
row boundaries never constrain lane occupancy; phase 2 applies the
row structure as unit-stride reductions over `t` via `ia[]`. The
gather addresses are byte-identical to row-based CSR — only the
announcement structure and lane utilization change, which makes
CSR → JDS → flat a controlled announcement-granularity series for
announcement-driven prefetchers (revela/vhybrid, and vtyche's
`limit_gate`). Costs: 2·nnz extra memory traffic through `t`
(allocated outside the ROI, same preprocessing argument as the JDS
conversion), and unfused multiply-then-add accumulation versus the
CSR kernel's fused `vfmacc` (covered by the verifier's 1e-6
tolerance; the serial flat variant matches `spmv_serial` bitwise).

**Validation.** Host-compiled serial build passes `.verif`
comparison on all four matrices in all three formats; gem5 smoke run
(football.csr, `flat`, vector binary) prints `Verification pass`.
All three binaries (`spmv_vector/serial/autovec.exe`) rebuilt
2026-08-16 with the same GNU toolchain flags as the 2026-08-12
builds.

### Limit-aware index slicing (`limit_aware_slicing`, 2026-08-13)

**The existing gem5 mechanism.** Prefetch and fill traffic is
line-granular: VHybrid's conversion drain (`convertLine`,
`src/mem/cache/prefetch/vhybrid.cc`) inherited VTyche's whole-line
semantics and pushed every raw word of a captured 64B index line
through the shift-and-add array. Nothing in the cache tells the
prefetcher where an ARRAY ends inside a line.

**What this fork adds.** The announcement already knows: the STT
entry's `limitAddr` (`src/cpu/revela_table.hh`, from the vsetvl AVL)
is the index array's end. A captured line straddling it has tail
bytes that are adjacent NON-index data — each would previously be
interpreted as an index and become a garbage target at
`base + (extend(raw) << shift)` (mostly self-filtered today by the
canonical-address check and cross-page translation drops, but at the
cost of MMU work, queue slots and dedup-window entries). Now each
capture registration records `validBytes = min(64, limit@ − line@)`
— on the prefetch-window path (`getPacket`) from the announcing STT
entry in hand, on the demand-miss path via `announcedLimit(pc, line)`
(smallest live `limitAddr` above the line for that producer PC; the
conservative choice under PC aliasing — a mis-attribution can only
suppress conversions, never fabricate targets). `convertLine` clamps
its slice loop to `validBytes / elemBytes`; suppressed elements are
counted in the `elementsBeyondLimit` stat. IMPORTANT design point:
the clamp is at the ANNOUNCED ARRAY extent, not the per-instruction
`vl` — clamping at each gather chunk's `vl` would amputate the
cross-chunk lookahead (converting the next row's indices early) that
makes the capture path timely on dense index arrays.

**Scope.** VHybrid only: this is inherently an announcement-driven
feature — standalone VTyche has no source for the array extent.
Expected magnitude is one line-tail per announced extent (negligible
on a single dense `ja[]`; material for many-short-array shapes such
as JDS diagonals or tiled formats). `--pf-param
limit_aware_slicing=false` restores whole-line conversion for
ablation and for reproducing pre-2026-08-13 runs.

### Conversion lane budget (`conversion_lanes`, 2026-08-13)

**The existing mechanism.** VHybrid inherited VTyche's conversion
width: the drain pushed one WHOLE captured index line through the
shift-and-add array per event (`emissionLineLimited` counts events
that had further lines waiting). The array's width — how many
`base + (extend(idx) << shift)` circuits exist — was not a knob;
effectively it equaled the line's element count (8 at e64, 16 at
e32).

**What this fork adds.** `conversion_lanes` bounds the ELEMENTS
converted per drain event, the direct analog of GDP's
`pipelines_per_gather` replay-width budget (`gdp.cc`, 2026-07-29
sweep). `CapturedLine` gains a conversion cursor (`nextElem`): a
line wider than the budget stays at the slice-buffer front and
resumes on later events; `conversionWidthLimited` counts the
cut-shorts (read against `emissionDeferred`/`emissionLineLimited` to
see which budget binds). Semantics preserved across the split:
within-line dedup carries through `PipeEntry::lineSeen` (a chunked
line emits exactly the distinct target lines whole-line conversion
would have), and a dedup-window entry still spans one SOURCE line —
`pendingEmitted` accumulates across chunks and closes with the
line's last chunk (`ConvertedLine::lastChunk`). The lane budget
composes with `limit_aware_slicing`: the clamp shortens the element
range, the budget paces it. `0` = unbounded (pre-2026-08-13
behavior, the default); `8` = one full e64 line per event; `1` = the
strictly serial pipe.

### Queue-full drop policy (`drop_on_full`, 2026-08-13)

**The existing gem5 mechanism.** VHybrid's one prefetch queue
(`Queued::pfq`, `mem/cache/prefetch/queued.cc`) carries BOTH traffic
classes — stream lines and converted indirect targets. On a full
queue, `Queued::insert` makes room by evicting the queue's oldest
entry (`pfRemovedFull`), and VHybrid's `drain_floor` deliberately
licenses up to `drain_floor` such displacing pushes per demand event
(VTyche's bounded-displacement anti-starvation; `drain_floor=0` is
the stall-on-full ablation that collapsed GDP v2.1 on poisson3Db).
Consequence: a burst of converted targets can evict staged STREAM
prefetches — visible as vhybrid's `pfRemovedFull` 49K vs vtyche's
6.5K in the Aug-12 JDS head-to-head.

**What this fork adds.** `drop_on_full` (default false) is the third
disposition at the same decision point: on a full queue the incoming
TARGET is discarded (`targetsDroppedFull`) and the queue keeps its
entries; the latch always drains, so no backpressure builds into the
slice buffers (unlike stall). The `drain_floor` room-forcing is
bypassed when set — the two are alternative answers to the same
question. A dropped target is NOT recorded in the dedup window
(`pendingEmitted`): no prefetch went out, so a later duplicate of
that line may legitimately re-emit and succeed. The three policies:
displace (newest-wins, default), stall (preserve conversion work,
backpressure), drop (oldest-wins, free-flowing pipe).

### vl-aware dedup scope and pre-lane placement
### (`vl_window_dedup` / `vl_window_bytes` / `pre_lane_dedup`, 2026-08-14)

**The existing gem5 mechanism.** VHybrid's within-conversion dedup
(`PipeEntry::lineSeen`) is scoped to one captured index line, and the
cross-line window (`dedup_buffer_size`) to a tuned number of index
lines; both compare TARGET line addresses after the shift/add
conversion. The deeper window's suppressions are a residency bet: a
duplicate is dropped on the assumption its line is still in
cache/MSHR when the later demand arrives.

**vl scope (`vl_window_dedup`, default false).** RVV gives one dedup
scope a hard guarantee: elements of a SINGLE gather instruction that
share a target line are coalesced by the LSQ onto one MSHR no matter
the cache state, so suppressing their duplicate prefetches is always
safe. The vl window realigns the seen-set to that boundary — it
clears when the element VA crosses a `vl_window_bytes` bucket
(default 256 B of index stream = one full chunk at VLEN=2048; a full
chunk always consumes VLEN/8 index bytes regardless of EEW, since
vl x elemBytes = VLEN/8). Buckets are VA-aligned: exact instruction
boundaries when the index array is chunk-aligned, else a constant
phase shift (and capture gaps cannot desynchronize it, unlike an
element counter). Stream tails shrink the window automatically via
the `limit_aware_slicing` clamp. The `dedup_buffer_size` window
stays orthogonal; `vl_window_dedup` with dd=0 is the pure
"suppress only what the ISA guarantees" design. Hardware cost: one
<= VLEN/EEW-entry CAM cleared per chunk, replacing per-line
bookkeeping.

**Pre-lane placement (`pre_lane_dedup`, default false).** Two
indices map to the same target line iff
`((extend(idx) << shift) + (base & 63)) >> 6` is equal — derived by
dropping the line-aligned part of the base from
`(base + (idx << shift)) >> 6`. Extension is wire fan-out, the shift
is wire selection, and the base's sub-line offset (nonzero for
sub-mmap-threshold mallocs, folded `&A[k]` bases, and always for
AoS field gathers) is a per-form constant folded at CAM insertion —
no full-width base add. The compare therefore runs BEFORE the lane
array: a hit (`elementsPreDeduped`) consumes no shift/add circuit
and no `conversion_lanes` budget slot, so a narrow lane array spends
its budget only on distinct target lines (on poisson3Db JDS ~32% of
elements are window-suppressible, i.e. a 2-lane array does ~3 lanes
of distinct work). Composes with either scope; filtered
(non-canonical) targets never enter the seen-set in either
placement.

**Validated 2026-08-14 (poisson3Db JDS trio):** default path
bit-exact (12,390,026 cycles); vl window catches 181,408 duplicates
vs 99,848 per-line (the predicted ~dd4 level) at noise-level cycle
cost; pre-lane moves all of them ahead of the lanes
(elementsPreDeduped 181,200, targetsDeduplicated 0) and cuts
conversionWidthLimited 880,822 -> 832,055 at conversion_lanes=2 —
duplicates measurably stop consuming lane budget.

### Stream emission floor (`initial_distance`, revela + vhybrid, 2026-08-15)

#### What the existing method did

ReVeLA's trigger (ported in `src/mem/cache/prefetch/revela.cc`
`emitRound()`, and reused by vhybrid's stream half in
`src/mem/cache/prefetch/vhybrid.cc` `emitStreamRound()`) always ramps
each stream's emission frontier up from the demand cursor. The
CPU-side STT (`src/cpu/revela_table.cc`) initializes
`prefetchedUpTo = currentAddr` at stream allocation (line ~147) and
snaps it back up to `currentAddr` whenever demand overruns the
frontier (line ~111) — so at every stream entry AND after every
demand catch-up, the stream re-enters at prefetched distance zero.
Lines emitted at distance ~0 are doomed-late: demand reaches them
well inside one memory round trip, so they merge into the demand's
MSHR (`pfHitInMSHR`, 85K in the s7 poisson3Db-JDS l1/m32 run) and
hide little or no latency. Worse, the paper's min-distance-first
fairness (revela Figure 7) gives exactly these doomed-late emissions
top priority — a stream at distance 0 always outranks a healthy
stream at distance 12 for the degree-limited emission slots.
VTyche's stream side never has this phase: its `streaming_distance`
enters every stream at a fixed offset ahead of demand.

#### What this fork adds

A per-prefetcher emission floor, `--pf-param initial_distance=N`
(default 0 = paper behavior, bit-exact) on both `revela` and
`vhybrid`. `applyInitialDistance()` (revela.cc / vhybrid.cc) runs in
the emission round's stream scan: if a stream's frontier is closer
than N lines to `currentAddr`, it jumps to
`currentAddr + N*blkSize` (clamped to `limitAddr`), and the skipped
lines are counted in the new `linesSkippedFloor` stat. The floor is
applied prefetcher-side so the shared `RevelaStreamTable` semantics
are untouched. Interaction with the aggressivity table:
`aggressivityLimit()` now returns at least `initial_distance` —
without this, a floor above the (stream-count-divided) ceiling would
throttle a floored stream forever; with it, such a stream degenerates
to a fixed-offset follower, which is the intended vtyche-like
behavior. Skipped lines are never prefetched (demand pays their full
miss — the coverage cost of the knob); on vhybrid, skipped INDEX
lines still reach the conversion pipeline through the demand-miss
capture registration path (vhybrid.cc `notify` path, line ~862), so
gather targets self-heal, just late — exactly like the skipped
region itself.

## VTyche: announced-limit gate (`limit_gate`, 2026-08-15)

### What the existing method did

VTyche's index-array walk (`src/mem/cache/prefetch/vector_tyche.cc`
`streamAhead()`) is demand-anchored and extent-blind: on each
producer chunk event at `addr` it emits lines up to
`addr + size*(streaming_distance+1)` with no notion of where the
index array ends, so at every extent end it (a) prefetches up to
`streaming_distance` chunks of adjacent non-index data and (b)
registers those lines for capture, converting every raw word of the
straddling line into a (garbage) gather target (`convertLine()`
sliced the whole payload unconditionally). VHybrid avoids both by
construction — its streams come from vsetvl-AVL announcements with a
known `limitAddr`, and `clampBytes()` slices the straddling line —
but that made extent semantics inseparable from the announcement-
driven stream engine.

### What this fork adds

`--pf-param limit_gate=true` on `--prefetcher vtyche` (default
false = bit-exact pre-gate behavior): an opt-in subscription to the
same `RevelaStreamTable` announcement sideband
(`src/cpu/revela_table.hh`) that revela/vhybrid use.
`prefetcher_factory.py needs_revela_table()` is now params-aware, so
the table (CPU `revela_table` + prefetcher `stream_table`) is wired
only when the gate is requested — plain vtyche runs keep their
table-free config, including scalar-side split-hierarchy placement.
Two gate points, both keyed by `announcedLimit(pc, va)` (vhybrid's
smallest-covering-limit aliasing rule, copied): `streamAhead()`
stops the walk at the announced extent end (new stat
`streamLimitClamped`), and capture registration stores
`gateBytes()` = the bytes below the limit into the IRT entry /
`CapturedLine.validBytes`, which the conversion (`convertChunk()`,
renamed from `convertLine()` 2026-08-16) uses to slice only
in-extent elements (new stat `elementsBeyondLimit`). Lines with no
covering announcement get `validBytes=0` = whole-line conversion, so
the gate is a strict no-op on announcement-blind code (CSR spmv).
This isolates ReVeLA's software contract as a single ablation
variable on top of the discovery design: measured headroom on the
current suite is small (vhybrid's `elementsBeyondLimit` was 424 of
2.37M elements on poisson3Db JDS; walk overrun is bounded by
`streaming_distance` lines per extent), which is itself the point —
the contract's extent information adds little that demand-anchored
discovery does not already provide.

## VTyche: VHybrid's conversion/emission knobs ported (2026-08-16)

`--prefetcher vtyche` now accepts the four conversion/emission
parameters VHybrid introduced, so the two designs can be swept on the
same axes: `conversion_lanes`, `drop_on_full`, `vl_window_dedup` (+
`vl_window_bytes`), and `pre_lane_dedup`. All default off/0 — a run
without these `--pf-param`s behaves exactly as before, so existing
sweep numbers stay comparable. VTyche2 is untouched.

### What gem5/VTyche did before

`vector_tyche.cc convertLine()` converted a captured index line
whole — one shift-and-add lane per element, all in one drain event —
with a batch-local duplicate scan on target line addresses, and
`drainEmission()` forced the emission budget up to `drain_floor` when
the queue was full (targets displace queued entries via
`Queued::insert`, `mem/cache/prefetch/queued.cc`). The dedup-window
contribution of a line lived inside the output latch
(`ConvertedLine::emitted`).

### What changed (mechanics follow `mem/cache/prefetch/vhybrid.cc`)

- `convertLine()` became `convertChunk()`: `CapturedLine` gained a
  `nextElem` conversion cursor and `ConvertedLine` a `lastChunk`
  flag. At `conversion_lanes=N`, at most N elements fire per drain
  event; an unfinished line stays at the slice-buffer front and
  resumes next event (`conversionWidthLimited`). At the default 0
  the whole line converts in one event, exactly as before.
- The within-line seen-set moved from a batch-local scan to
  `SttEntry::lineSeen`, so dedup keeps whole-line semantics across a
  lane-budget split; `vl_window_dedup` rescopes it to
  `vl_window_bytes`-aligned index-VA buckets (~one gather chunk,
  `SttEntry::windowKey`), and `pre_lane_dedup` moves the compare
  before the lanes onto truncated folded index keys (`preLaneKey()`;
  hits count `elementsPreDeduped` and spend no lane-budget slot).
- The dedup-window contribution moved to `SttEntry::pendingEmitted`
  and closes with the line's LAST chunk, keeping one window entry
  per source line under a lane split.
- `drop_on_full=true` disables the `drain_floor` displacement
  license in `drainEmission()`: overflow targets are dropped
  (`targetsDroppedFull`) and the queue keeps its staged entries.
  Dropped targets are not recorded in the dedup window, so a later
  duplicate may re-emit.

New stats: `elementsPreDeduped`, `conversionWidthLimited`,
`targetsDroppedFull` — same meanings as VHybrid's. Parameter
semantics are documented in full on `VectorTychePrefetcher`
(`src/mem/cache/prefetch/Prefetcher.py`); the factory forwards them
generically (`--pf-param conversion_lanes=8` etc.,
`rvv/prefetcher_factory.py`).

### Behavior notes

- Interactions with v1-only features compose: the `limit_gate` byte
  clamp applies before the lane budget (the beyond-limit tail is
  charged once, on the line's first chunk), and `row_schedule_bits`
  operates downstream on the queue, unaffected.
- One deliberate difference from the pre-port code: a latch discarded
  by a staleness abort or stale form now also forfeits its
  half-built dedup-window contribution (`lineSeen`/`pendingEmitted`
  clear), matching VHybrid; before, a partially-emitted latch's
  contribution was silently lost the same way, so observable
  behavior at default parameters is unchanged.

## VTyche: multi-way indirect support (`consumers_per_producer`, 2026-08-16)

One producer index load feeding SEVERAL gathers (`A[B[i]] + C[B[i]]`,
the SoA multi-way pattern) is now prefetched for every target array:
`--pf-param consumers_per_producer=M` (default 1). VTyche2 and GDP are
untouched.

### What the table/prefetcher did before

The DCT head row held ONE consumer pointer, and backward propagation
overwrote it at every gather dispatch (`vector_chain_table.cc`,
backprop block) — a two-gather loop ping-ponged the slot every
iteration (`linksFormed` churn), and vtyche's single `LinearForm` per
`SttEntry` converted each captured index line for whichever gather won
the race. The PT (forward propagation) was never the limitation: both
gathers already read the same provenance entry.

### What changed

- `VectorChainTable` (`src/cpu/vector_chain_table.hh/.cc`,
  `VectorChainTable.py`): the head row's `int consumer` became up to
  `consumers_per_producer` slots (~4 bits each: 3-bit DCT pointer +
  empty encoding). At capacity 1 backprop keeps the historical
  overwrite bit-for-bit; at capacity > 1 it is insert-if-absent, a
  distinct gather beyond capacity bouncing off (new stat
  `consumerSlotsExhausted`). `producerInfo()` reports `numConsumers`;
  `pipelineConfig()`/`chainSnapshot()` take a slot argument
  (slot-0 default keeps GDP/VTyche2 call sites unchanged). PT, clear
  semantics, generation counter, stream-page registry: untouched.
- VTyche (`src/mem/cache/prefetch/vector_tyche.hh/.cc`): per-way
  `ConsumerGroup` state {form, lineSeen, pendingEmitted, dedupWindow}
  replaces the flat single-form fields; the slice buffer, IRT,
  stream walk and `windowKey` (an index-VA property) stay SHARED —
  the index side is never duplicated, the captured raw line
  broadcasts to every way's shift/add lane group. Adoption walks all
  linked slots, ways configuring independently. The latch is
  element-major interleaved (`A[B[0]], C[B[0]], A[B[1]]...`) with a
  per-target group tag routing per-way dedup/stale handling at
  emission. `conversion_lanes` counts ELEMENT slots shared across
  ways, charged on any-miss: with `pre_lane_dedup` every way's CAM
  compares before lane allocation and an element every way gates on
  is compacted out free (new stat `slotsCompacted`). Whole-latch
  staleness flush requires ALL ways stale (`anyFresh`); a single
  stale way forfeits only its own targets.
- Wiring: the prefetcher's `consumers_per_producer` pf-param is the
  single source of truth; `vector_cache_hierarchy.py` /
  `unified_cache_hierarchy.py` copy it onto the `VectorChainTable`
  they construct (guarded on `_params`, so gdp/vtyche2 keep 1).
- Benchmark: `rvv/vector_indirect_eval.c` gained a trailing `ways`
  arg (1|2). At 2 a second 64 MiB array C is gathered with the same
  offset vector in the same loop (the 1-way loop is untouched);
  C[j] = 2*j so the expected ROI sum is exactly 3x the 1-way sum.
  Host-validated (scalar fallback) and gem5-validated.

### Validation (2026-08-16, vlen 2048 / throughput 4 / simd-units 2, vtyche on vector L1)

- `gdp_test.bin` m=1, pre-change binary vs post-change binary:
  **0 differing common stats** (7,005,881 cycles both) — the
  restructure plus the same-day conversion-knob port are
  default-neutral, bit-for-bit.
- `gdp_test.bin` (single gather) m=1 vs m=2 on the new binary:
  **0 differing stats** — unused capacity is free.
- `vector_indirect_eval` `line_locality 65536 4 33554432 1 1 2`
  (2-way SoA), ROI stats:

| stat | m=1 (control) | m=2 |
|---|---|---|
| `linksFormed` | 4,096 (churn: 2/iteration) | **2** (one per gather, stable) |
| `elementsConverted` | 64,480 | 130,496 (2x: both ways) |
| `targetsGenerated` | 16,112 | 32,608 |
| `pfIssued` / `pfUseful` | 24,304 / 23,108 | 40,762 / 33,811 |
| vector-L1D `overallMisses` | 70,454 | **26,983** (-62%) |
| ROI `numCycles` | 734,566 | **436,367** (1.68x) |

`consumerSlotsExhausted` 0 at m=2; kernel result PASS in every run.
Raw runs in `rvv/out_multiway_val/`.

## VectorChainTable sizing knobs exposed to the config script (2026-08-16)

`--vector-dct-entries` and `--vector-max-transform-stages` make the
CPU-side chain table's capacity sweepable from the command line. They
size the table for whichever chain-table prefetcher is attached —
`gdp`, `vtyche`, `vtyche2` and `vhybrid` all share ONE
`VectorChainTable` per core, so vhybrid's indirect half is covered by
the same two flags. Defaults (8 / 4) are the previous hardcoded
values, so every existing command line is bit-neutral.

### What the existing method was

`VectorChainTable` (`src/cpu/VectorChainTable.py:27`) has always had
`dct_entries` (default 8) and `max_transform_stages` (default 4) as
`Param.Unsigned`, but nothing could set them. The config scripts
constructed the table with only the stream-demotion fields:

```python
tbl = VectorChainTable(
    demand_stream_pages=self._stream_demote_demand,
    monotone_arm=self._stream_demote_monotone,
)
```

(`rvv/vector_cache_hierarchy.py`, `rvv/unified_cache_hierarchy.py`).
`--pf-param` was not an escape hatch either: it validates names
against the selected *prefetcher's* `_params` and errors on anything
else (`rvv/prefetcher_factory.py:165`), and these two live on the
table SimObject, not the prefetcher. The DCT was therefore fixed at 8
entries in every run to date.

### What this fork adds

Plumbing only — no C++ change, following the `--revela-stt-entries`
precedent (`RevelaStreamTable`, the same shape of CPU-side sideband
table):

- `rvv/riscv-rvv-se-ara-prefetcher.py`: the two flags, plus argparse-
  time guards (`>= 3` / `>= 1`) so the failure names the flag instead
  of hitting the C++ `fatal_if` in `vector_chain_table.cc:34`.
- `rvv/vector_cache_hierarchy.py`, `rvv/unified_cache_hierarchy.py`:
  new ctor kwargs `vector_dct_entries` / `vector_max_transform_stages`
  stored on the hierarchy and passed at BOTH `VectorChainTable`
  construction sites in each file — the prefetcher-wiring site in
  `_finish_prefetcher()` and the demand-only site that builds a table
  with no prefetcher for `--stream-demote --stream-demote-demand`
  runs.

### What the two knobs actually control

- `dct_entries` — chain-table capacity. `dctInsert()` clears the WHOLE
  table and bumps the generation counter when it fills (Tyche's
  `clear_dct` semantics), so a small DCT wipes learned chains more
  often. It also bounds the backward-propagation walk that finds a
  chain's head (`hops < dctEntries`, `vector_chain_table.cc:442`).
- `max_transform_stages` — separate cap on transform links between
  producer and gather (`rev.size() >= maxTransformStages`,
  `vector_chain_table.cc:734`); an over-long chain returns an invalid
  snapshot and simply does not link. Raising `dct_entries` alone will
  not admit longer transform chains.

Neither is packed in the simulator: `lastDctPtr` and the consumer
slots are `int`, and `ChainSnapshot::ops` is a `std::vector`, so any
value runs. The "3-bit pointers" note in
`src/cpu/vector_chain_table.hh:17` is hardware storage accounting
pinned to the default of 8 — the ~961 B figure for GDP's tables needs
redoing if a larger DCT is adopted as the design point.

### Validation (2026-08-16, `gdp_test.bin`)

Config-only change, verified by reading back `config.ini`:

| run | flags | `config.ini` |
|---|---|---|
| split hierarchy, vhybrid on vector L1 | `--vector-dct-entries 16 --vector-max-transform-stages 6` | `dct_entries=16`, `max_transform_stages=6` |
| unified hierarchy, vhybrid on L1 | `--vector-dct-entries 24 --vector-max-transform-stages 3` | `dct_entries=24`, `max_transform_stages=3` |
| demand-only table (`--prefetcher none --stream-demote l2 --stream-demote-demand`) | `--vector-dct-entries 12` | `dct_entries=12`, `max_transform_stages=4` |

All three ran to completion; the split-hierarchy run printed
`gdp_test: ... -> PASS`. `--vector-dct-entries 2` exits with the
argparse-time guard message.

## TSVC MEDIUM campaign driver (`rvv/tsvc_sweep.py`, 2026-08-16)

The 8/16/26 sweep plan (four prefetcher arms x 4 kernels x 2 iteration
counts x 2 ip[]-locality pairs x 2 MSHR pairs) needed a driver, and
running it surfaced two TSVC bugs that only appear at MEDIUM.

### What the existing method was

`tsvc-gem5/run.sh` / `run-subset.sh` drive one kernel per gem5
invocation against the OLD config script (`riscv-rvv-se-ara.py`, no
prefetcher support), and `spmv_param_sweep.py` /
`vector_indirect_sweep.py` drive prefetcher sweeps but are wired to
RiVec spmv and `vector_indirect_eval` inputs. Neither could express a
TSVC prefetcher campaign.

### What this fork adds

`rvv/tsvc_sweep.py`, following the established sweep conventions
(DONE/FAILED markers per run dir, resumable, `--jobs`, `--only`,
`--collect-only`, `GEM5_SWEEP_RUNNER=native` for kazi). Two things are
specific to TSVC:

- **One invocation runs all four kernels.** TSVC's `init()`
  (`common.c`) builds a LEN_1D Fisher-Yates permutation before any ROI,
  which at MEDIUM costs ~2.5 min of host time regardless of which
  kernels the argv filter selects (`should_run`, `tsvc.c`). Batching
  turns 232 kernel-ROIs into 58 invocations. It is state-equivalent to
  separate runs, not a confound: every prefetcher wipes learned state
  on the stats-reset callback (`base.cc:122` registers
  `resetLearnedState`) and `VectorChainTable` clears at ROI boundaries
  (`vector_chain_table.cc:37`), so each kernel's ROI still begins with
  a cold predictor.
- **Two stats dumps per kernel.** `ROI_END` is
  `m5_dump_reset_stats(); m5_work_end();` (`common.h:77`) and the
  work_end pseudo-op dumps again, so each kernel leaves its real ROI
  section followed by a ~5-instruction artifact; gem5's end-of-run dump
  adds one more after the last kernel. `kernel_sections()` filters on
  `simInsts >= 1000` and takes the first N — a naive
  `zip(KERNELS, sections)` misattributes every kernel after the first.

Checksums are parsed by field, not by shape: a finished kernel's line
carries the kernel name TWICE (`initialise_arrays` prints `"%5s\t"`
with no newline, then `time_function` prints
`"%-12s\t%12llu\t%f\n"`).

### Bug found: `set_1d_array` reciprocal-square overflow (fixed)

`common.c` computed `1. / (real_t)((i+1) * (i+1))` in `int`. At
i=65535 the product is exactly 2^32, wraps to 0, and the element
becomes `1.0/0.0 = inf`. That is s4113's `c[]` at MEDIUM and above, so
its checksum was `inf` — which would have made cross-config checksum
agreement pass trivially and give false confidence. Now squared in
`double`: exact for every LEN_1D we run, and bit-identical to the old
values at TINY and SMALL where the overflow never triggered (so no era
break for existing `tsvc_results/` data).

### Checksum validation strategy

Native x86 goldens are INVALID for the current TSVC: `ip[]` is a
rand()-based permutation (`common.c`, the locality generator added
2026-08-16) and glibc's `rand()` differs from the RISC-V build's
newlib `rand()`, so a native build produces an entirely different index
array. `generate_golden.sh` also never passes `-L/-W/-S`, so it cannot
target a locality pair even in principle. Two defenses replace it:

- **Cross-config agreement** (free): all runs sharing
  (kernel, L, W, iterations) must report the same checksum. The latent
  vle32-under-e64m2 widening-chain corruption is timing-dependent, so a
  prefetcher that perturbs timing would break agreement.
- **novec reference**: `tsvc_novec_MEDIUM` at each (L,W). s353/s4112/
  s4113 are element-wise and must match exactly; s4115 is a sum
  reduction, so vectorization order moves the low bits and it is
  compared with a relative tolerance.

## VTyche: read-port index capture (`capture_on_read_hit`, 2026-08-17)

An index line that is **already resident** in the cache when its demand
read arrives can now still be captured and converted:
`--pf-param capture_on_read_hit=true` (default `false`, so every
existing run is bit-identical). VTyche2, GDP and VHybrid are untouched.

### What the existing method did

VTyche's conversion engine is fed exclusively by *fills*. Two paths
register a line in the Index Routing Table (IRT) and then wait:

- the **prefetch-window** path in `VectorTyche::getPacket()`
  (`src/mem/cache/prefetch/vector_tyche.cc`) — a departing prefetch
  whose VA falls inside a configured producer's stream window is index
  data, so its line PA is registered (`capturesRegistered`);
- the **demand-miss** path in `calculatePrefetch()` — an index-array
  demand miss carries both VA and PA, so its lines are registered
  directly (`capturesRegisteredMiss`), explicitly *skipping* lines that
  `cache.inCache()` reports resident, since no fill will come for them.

`notifyFill()` then matches the arriving fill's block PA against the
IRT, erases the entry, and latches the payload into the owning
producer's slice buffer (`fillsCaptured`).

The gap: **a resident line never fills**, so a registration taken for
one is dead weight that ages out of the bounded IRT deque without ever
producing index data. This is the dominant capture loss on any loop
where several producer PCs walk the same index array, because each
producer's stream walk covers its siblings' slices and makes their
index lines resident *before* their own demand loads reach them.
Measured on TSVC `s353` (five interleaved gather chains) with
`pipelines=5`, vector L1, MSHRs (16,20), L=1 W=1:

| | pipelines=2 | pipelines=5 |
|---|---|---|
| `pipelinesSaturated` | 6384 | 0 |
| `capturesRegistered` | 1901 | 18740 |
| `fillsCaptured` | 2021 | **4855** |
| `elementsConverted` | 27920 | 62800 |
| `coverage` | 0.105 | 0.140 |

Lifting the pipeline lockout registered 9.9x the lines but latched only
2.4x — **74% of registrations could never latch.** `ip[]` is 1 MB =
16384 distinct lines, so the fill path was reaching under a third of
them.

### What this fork adds

A third capture path that sources the payload from the cache read port
instead of a fill:

- `notify()` (already overridden to latch the drain's translation
  context) also latches the in-flight access packet into a transient
  `hitPkt` member when the access is a satisfied demand **read hit**,
  and clears it as soon as `Queued::notify` returns. The gate is the
  same predicate the core uses to decide a read hit carries data
  (`PrefetchInfo`'s constructor, `src/mem/cache/prefetch/base.cc` — the
  IMP read-hit fix): `!isCacheMiss() && pkt->isRead() &&
  !pkt->cmd.isHWPrefetch()`, plus `pfi.hasData()`. This is sound
  because `access()` copies the block into the packet
  (`satisfyRequest` -> `setDataFromBlock`) *before* the Hit probe fires
  (`src/mem/cache/base.cc`), so the bytes are readable in the callback.
- `captureFromReadHit()` latches each whole resident line the access
  covers straight into the producer's slice buffer, reusing the fill
  path's `CapturedLine` staging, freshness gate (`anyFresh`) and
  buffer-full shedding (`bufferBusyDrops`). New stat: `hitsCaptured`.
- `dropRegistration()` retires any IRT entry for a line captured this
  way, so a later fill cannot double-convert it.

No core file changed. The read hit hands over the payload, the VA and
the PA simultaneously, so this path needs **no IRT round trip at all** —
the registry exists only because fill packets are PA-only and arrive
asynchronously.

### What it does and does not buy

It buys *capture*, not *lookahead*. Index line X lives in exactly one
producer's slice, and the hit happens at that producer's own index
load — roughly eight instructions before its gather. The mechanism is
therefore MLP expansion (one captured line becomes up to
`blkSize/elemBytes` target requests issued as a burst, versus the LSQ
issuing gather elements serially through a handful of MSHRs), plus the
fact that the sibling chains get index data *at all*.

`capture_on_read_hit` is only useful together with a `pipelines` value
at least as large as the number of concurrent index producers: capture
is gated on `anyConfigured(ps)`, so producers denied a pipeline never
register and never reach either capture path. On `s353` that means
`--pf-param pipelines=5 --pf-param capture_on_read_hit=true`.

Sweep 6 of `rvv/tsvc_sweep.py` (`s6_vtychep5rh_*`, sheet
`vtyche_p5rh`) is sweep 5's grid with the flag added, so the two sheets
diff cell-for-cell.

## Feature: PageRank benchmark — vectorizable graph workload (2026-08-17)

**What gem5/RiVEC provided.** Nothing: the RiVEC suite has no graph
benchmark (its 12 kernels are dense/sparse linear algebra and PARSEC
ports), and the Ligra `PageRank_riscv` build in `~/ligra` is a scalar
`edgeMap` framework kernel with no correctness check
(`~/ligra/RVV_BUILD_NOTES.md`).

**What was added.** A validated, intrinsics-vectorized PageRank at
`~/riscv-vectorized-benchmark-suite/_pagerank/`, following `_spmv`'s
conventions exactly (magic-sniffed binary input, `.verif` elementwise
comparison, `#ifdef USE_RISCV_VECTOR` kernel split, `Makefile.gcc`
registration, ROI markers from `common/roi.h`).

Formulation: the iterative-SpMV power method of EECS-2019-6
("Nested-Parallelism PageRank on RISC-V Vector Multi-Processors",
Amid 2019, §1.2), `y(k) = d·P·y(k−1) + (1−d)/|V|`, with the exact
semantics of the GAP Benchmark Suite's `pr_spmv.cc` (Jacobi pull:
damping 0.85, init 1/N, L1-error convergence, defaults 20 iters /
tol 1e-4) so results cross-validate against gapbs. Per iteration:
phase 1 is a unit-stride `vfmul` (`contrib = scores × inv_outdeg`,
reciprocals precomputed outside the ROI to avoid ARA's serial FP
divider); phase 2 is a per-vertex strip-mined `vle64`(indices) →
`vsll` → `vloxei64` gather → ordered `vfredosum` over the in-neighbor
list — `_spmv`'s CSR shape minus the values array, i.e. a pure
gather-reduce with a u64 index stream (`ja[]`), the pattern the vector
prefetchers target.

Files: `src/graph_format.h` (magic `PGRKCSR1`; u64 N, NZ, in-CSR
`ia`/`ja`, `outdeg`), `src/pagerank.c` (serial + intrinsics one-
iteration kernels), `src/main.c` (loader with dense-`.el` text
fallback, ROI around the whole power iteration, buffered per-iteration
error prints, NaN-proof `.verif` compare), `el2pr.c` (host converter:
SNAP edge list → dense-id remap → sorted in-CSR image + float64 golden
`.verif` + optional remapped `.el` dump for gapbs).

Inputs (in `_pagerank/input/`): `tiny` (64-vertex irregular synthetic,
smoke tests), `wiki-Vote` (SNAP, 7115 v / 103689 e — EECS-2019-6's
smallest evaluation graph; converges in 17 iterations), `amazon0302`
(SNAP, 262111 v / 1234877 e — EECS-2019-6's medium graph; runs the
full 20 iterations).

Validation chain: (1) converter float64 golden vs an independent
Python implementation — every printed digit matches (sum
0.4176928988, top vertex original id 4037 = the published SNAP result
for wiki-Vote); (2) vs GAP's `pr_spmv` (float32) run on the remapped
edge list — max per-vertex |Δ| 1.3e-9, identical top-5; (3) host-native
serial run verifies all three inputs including the text-fallback path;
(4) gem5 ARA runs (VLEN 512, chaining) verify serial and vector
variants; vector output is bitwise identical to serial because the
strip reductions are ordered and seeded left-to-right.

**gem5 pitfall found on the way (affects `_spmv` too).** This gem5
implements the RVV tail-agnostic policy by writing all-ones — a NaN
bit pattern — into tail lanes. `_spmv`'s `spmv_intrinsics` idiom
(accumulate lane partials with a shrinking `vsetvl`, then one
`vfredosum` re-widened to `VSETVL(nnz_row)`) therefore reads clobbered
lanes for every row whose nnz is > VLMAX and not a multiple of VLMAX,
and its result is NaN (confirmed by a per-row replica on football.csr:
26/115 rows NaN at VLEN=512). `_spmv`'s verification does not catch
this because `fabs(nan − ref) > TOL` is false — a NaN passes. The
pagerank kernel avoids both traps: it carries the ordered reduction
across strips through the scalar seed (`part_res[0]`), never reading
tail lanes, and its verifier uses the negated comparison
`!(fabs(Δ) <= TOL)` so NaN fails. `_spmv` itself was left untouched
(fixing it changes the kernel's ARA work profile and would break
comparability with collected timing series — the spmv address stream
does not depend on the corrupted `y[]` values, so prefetcher/timing
numbers remain meaningful); apply the same two fixes there if value
correctness is ever needed.

Build & run:

```bash
cd ~/riscv-vectorized-benchmark-suite
make -f Makefile.gcc pagerank_serial pagerank_vector pagerank_autovec

# host-side input prep (already done for tiny/wiki-Vote/amazon0302):
cd _pagerank && gcc -O2 -o el2pr el2pr.c -lm
./el2pr input/wiki-Vote.txt input/wiki-Vote.pr input/wiki-Vote.verif

# gem5 (from /gem5 in the docker container):
build/RISCV/gem5.opt rvv/riscv-rvv-se-ara-prefetcher.py \
    --enable-chaining --vlen 512 --vector-timing-throughput 4 --simd-units 2 \
    -p "/riscv-vectorized-benchmark-suite/_pagerank/input/wiki-Vote.pr \
        /riscv-vectorized-benchmark-suite/_pagerank/input/wiki-Vote.verif" \
    /riscv-vectorized-benchmark-suite/_pagerank/bin/pagerank_vector.exe
```

Program CLI: `<graph.pr|graph.el> [max_iters] [tol] [file.verif]` —
numeric args are positional (iters, then tol), any non-numeric arg is
the verif path. The `.verif` goldens correspond to the defaults
(20, 1e-4); regenerate with `el2pr -i/-t` when overriding. The ROI is
deterministic for a fixed input+params (early exit depends only on the
data), and per-iteration error lines are buffered in memory during the
ROI and printed after `ROI_END()` so no syscalls pollute the stats.


## VTyche: retire-don't-erase IRT tail (`capture_read_hit_own_producer`, 2026-08-19)

`capture_on_read_hit` (above) converts an index line off the read port
when the demand read hits. On a single-chain loop it converts every
index line **twice**, and the better the streaming half works, the more
of it there is. This adds a small retired tail to the Index Routing
Table so the second conversion can be identified and, optionally,
dropped: `--pf-param capture_read_hit_own_producer=false`.

### What the existing method did

`notifyFill()` (`src/mem/cache/prefetch/vector_tyche.cc`) located the
arriving fill's IRT entry and **erased** it:

```cpp
const IrtEntry rec = *it;
indexRoutingTable.erase(it);
```

After that erase the prefetcher retained no evidence that the line had
ever been converted. `captureFromReadHit()` therefore had no way to
distinguish two very different read hits on the same index line:

1. **Self-inflicted duplicate** — VTyche's own stream walk prefetched
   the index line, `notifyFill()` converted it, and the producer's
   demand read then necessarily *hits* on the line the prefetcher just
   installed. The read-hit path converts the identical elements a
   second time, generating a second, byte-identical batch of indirect
   targets.
2. **Genuine sibling capture** — a *different* producer's stream walk
   made the line resident, so this producer has never seen the data.
   This is the multi-chain (unrolled-gather) case
   `capture_on_read_hit` was written for.

The arithmetic from the TSVC LARGE campaign (`s4112`, one gather chain,
`LEN_1D` 1024000, 16 int32 indices per 64 B line = 64000 `ip[]` lines):

| | count | vs. 64000 lines |
|---|---|---|
| `fillsCaptured` | 64004 | 1.00x |
| `hitsCaptured` | 62659 | 0.98x |
| `elementsConverted` | 2015440 | **2.0x** `LEN_1D` |

Every element converted twice. The contrast with `s353` (five
interleaved chains, `formsAdopted` = 3) is what shows a *global*
`capture_on_read_hit` flag is the wrong granularity: there
`fillsCaptured + hitsCaptured` is only 0.9x registrations, i.e. the
read-hit path is load-bearing and must not be turned off.

### What this fork adds

A bounded retired tail beside the pending IRT — key only, no payload:

```cpp
struct RetiredEntry { Addr linePaddr; Addr producerPC; bool secure; };
std::list<RetiredEntry> retiredRoutingTail;   // FIFO, retired_tail_entries
```

- `retireRegistration(line_pa, producer_pc, secure)` records a
  conversion. A re-retirement of a record already present *splices* it
  to the back rather than duplicating it, so a line converted
  repeatedly does not age out under its own traffic and the tail holds
  one record per (line, producer).
- `wasConvertedBy(line_pa, producer_pc, secure)` is the CAM read.
- `notifyFill()` still erases the pending entry — it must, or a second
  fill would re-convert — but now calls `retireRegistration()`
  **after** the payload is actually latched. A fill dropped on a stale
  form (`staleConfigs`) or a full slice buffer (`bufferBusyDrops`)
  converted nothing, so it leaves no tail record and a later read hit
  on that line is still real work.
- `captureFromReadHit()` consults the tail before spending a slice
  buffer entry. The key includes `producerPC`, so case 1 above matches
  and case 2 does not.
- The read-hit path retires its own captures too, so a repeated read
  hit by the same producer on the same line is treated identically to
  a fill-then-hit pair.

New stats (`system.cpu.<cache>.prefetcher.`):

| Stat | Meaning |
|---|---|
| `recapturesDetected` | read-hit captures of a line this same producer had already converted — counted whether or not they were suppressed |
| `recapturesSuppressed` | of those, the ones actually skipped |

The tail is FIFO-bounded by `retired_tail_entries` (default 32, the
same order as `routing_entries` and as `streaming_distance` in lines).
Ageing out is deliberate, not a shortcoming: a line converted long
enough ago that 32 other conversions have intervened is past both the
`dedup_buffer_size` horizon and any plausible cache residency window,
so re-converting it on a genuine revisit is the correct answer — the
same reasoning as the dedup window's bounded scan
(`inDedupWindow()`).

### Behavior changes

None by default. `capture_read_hit_own_producer` defaults to `True`,
which is exactly the pre-tail behaviour, so every run in the TSVC
LARGE campaign and sweep 6 stays comparable. At the default the tail is
still maintained and `recapturesDetected` still counts, so a rerun
measures the size of the duplication without changing timing;
`recapturesSuppressed` stays 0 until the flag is turned off.

**Validated 2026-08-19** (MEDIUM, center params + pipelines=5 +
capture_on_read_hit, L8W32, `validation_aug19/V1` vs `V2`, checksums
golden): at the default, detected == hitsCaptured exactly on the
single-chain kernels (s4112 15,632) and **detected = 0 on s353** —
every one of its 9,458 read-hit captures is a genuine sibling capture,
so the per-producer key separates the two cases perfectly (V1 and V2
s353 rows are bit-identical). With suppression on: elementsConverted
2.0x -> 1.0x LEN_1D, pfIssued -33% (292K -> 195K), pfHitInCache -42%
(228K -> 132K), pfUseful flat (+0.4%), cycles flat (0.996-1.005) — the
win is bandwidth/energy-side, as expected on gem5's free-drop cache.
`retired_tail_entries=0` disables the tail entirely (no detection, no
suppression) and removes even that bookkeeping.

The flag only has an effect when `capture_on_read_hit=true`; with the
read-hit path off there are no read-hit captures to suppress.

### Storage

`retired_tail_entries` x (PA line tag + PC + secure bit). At the
default 32 entries with a 40-bit PA (34-bit line tag) and a 40-bit PC:
32 x 75 b = 2400 b = **300 B**, on top of the ~961 B already accounted
for the rest of the design.


## VTyche: capture admission backpressure (`capture_queue_gate`, 2026-08-19)

`--pf-param capture_queue_gate=N` skips new captures while the prefetch
queue backlog is at or above N entries. Default 0 = disabled, so every
existing run is bit-identical.

### What the existing method did

VTyche has backpressure at **emission** — `drainEmission()` stalls
against a full `pfq` (`emissionDeferred`), and v2.1's stall-on-full
replay keeps half-emitted batches parked — but none at **capture
admission**. The walk keeps registering index lines
(`registerCapture()` in `getPacket()` and `calculatePrefetch()`,
`src/mem/cache/prefetch/vector_tyche.cc`) and the fill/read-hit paths
keep latching payloads into slice buffers regardless of whether their
targets could ever issue. When the queue is saturated, batches sit in
the slice buffer until the demand cursor passes their index line, and
the staleness abort discards them wholesale (`stalenessAborts`).

Measured in the TSVC LARGE campaign (see `tsvc_large_results/`): at
L1W1 the pfq (queue_size 64) is chronically full — `emissionDeferred`
1.9–2.5M events against ~800K drain ticks — and **~50% of all captured
lines are staleness-aborted** in many configs (median 13% campaign-wide,
56% worst case at cl1-sd32 s353 L8W32). Every one of those was a
registration, a fill or read-hit latch, and sometimes a partial
conversion, spent on a batch that was then thrown away.

### What this fork adds

One predicate at the admission points (three when written; the
demand-miss registration path was removed the same day, see the next
section):

```cpp
bool VectorTyche::captureBacklogged() const {
    return captureQueueGate != 0 &&
           pfq.size() + pfqMissingTranslation.size() >= captureQueueGate;
}
```

- **Prefetch-window registration** (`getPacket()`): the departing
  stream prefetch still issues and `registerStreamPage()` still runs
  (the demotion registry is not capture work) — only the
  `registerCapture()` is skipped.
- **Read-hit capture** (`captureFromReadHit()`): the whole latch is
  skipped before any payload is copied.

The backlog counts `pfqMissingTranslation` as well as `pfq` because
indirect targets are nearly all cross-page (`pfSpanPage` ≈ 100% of
targets on TSVC), so the translation-pending queue is real pipeline
depth, not a corner case.

The stream walk is never gated: the architectural stream is the useful
readahead and its candidates go through the normal queue-full handling.
The gate sheds only the capture/convert half — exactly the work whose
product cannot issue while the queue is full.

New stat: `capturesGated` (admissions skipped, one count per line or
read-hit call).

### Sizing guidance

`queue_size - blkSize/index_bytes` admits a capture only when a full
line's worth of converted targets could actually queue: at the default
queue_size 64 with int32 indices, `capture_queue_gate=48`.

### What it does not fix

A gated line is not deferred, it is *lost* — if the queue drains before
the demand cursor reaches that line, the gate traded a would-have-been-
timely capture for nothing. The bet is that at sustained congestion the
staleness abort would have discarded it anyway; `capturesGated` vs the
change in `stalenessAborts` measures how often the bet pays.

**Validated 2026-08-19** (MEDIUM, L1W1, gate=48 vs off,
`validation_aug19/V3` vs `V4`): mechanically correct — stalenessAborts
collapse (s4112 10,340 -> 119) and capturesGated fires (~15K/kernel) —
but the bet only pays on some kernels: s4112 pfUseful +6%/cycles flat,
s4115 +9%/flat, **s4113 -17%/cycles +4.4%, s353 -38%/cycles +15.9%**.
capturesGated (15.4K) exceeded the aborts it replaced (10.3K): the gate
also discards captures that would have issued. gate=48 is NOT a
recommended setting on this evidence; the default (0 = off) stands, and
a useful version likely needs defer-not-drop or a higher threshold.


## VTyche: demand-miss IRT registration removed (2026-08-19)

The demand-miss capture path is deleted: `calculatePrefetch()` no
longer registers index lines in the IRT when the index-array demand
access misses. `registerCapture()` loses its `from_miss` flag and the
`capturesRegisteredMiss` stat is gone (`rvv/spmv_param_sweep.py`'s
scraper skips absent stats, and GDP keeps its own copy of both, so
nothing breaks).

### What the path was

One of the two IRT registration paths documented in the
`capture_on_read_hit` section (2026-08-17): an index-array demand miss
carries VA and PA together, so its lines were registered directly,
skipping `cache.inCache()`-resident lines. It existed to catch lines
the stream walk had not covered — stream startup, and whenever the
walk fell behind the demand cursor.

### Why it is unnecessary

- **It barely fired.** TSVC LARGE: `capturesRegisteredMiss` was 1-146
  per kernel at L8W32 and 1.4-5.5K at L1W1, against ~64K
  prefetch-window registrations - at most ~8% of registrations, in the
  regime (L1W1) where ~50% of captures were staleness-aborted anyway.
- **It captured exactly the doomed population.** A line the demand
  already missed on is a line the walk failed to cover in time; its
  fill arrives with the demand, its conversion targets issue *behind*
  the gather that needs them, and the staleness abort or
  `pfRemovedDemand` eats them. Registering it spends IRT slots (32
  total) that evict still-useful prefetch-window entries.
- **The walk self-heals.** The demand miss that would have registered
  is also the chunk event that advances `streamAhead()`, so the walk
  re-covers the region within one event either way.

Expected visible deltas on re-runs: `capturesRegisteredMiss` gone;
`fillsCaptured` down by roughly its old value in the L1W1 regime;
`stalenessAborts` down by a similar amount; `pfUseful` ~flat (the
removed captures were late by construction).

**Validated 2026-08-19**: `capturesRegisteredMiss` absent from stats,
all checksums golden (`validation_aug19/`, all four runs).


## VectorChainTable: vtmp/v0-v1 provenance aliasing fix (2026-08-19)

The chain table's provenance table (PT) indexed every vector RegId with
`& 31`. gem5's vector-memory macros stage their index operand through
the eight internal registers vtmp0..7 (`VecMemInternalReg0 = 32`,
`src/arch/riscv/regs/vector.hh`), so vtmp0/vtmp1 aliased onto
architectural **v0/v1's** PT slots. A gather's `VCpyVs` bookkeeping
micro, which deliberately copies index provenance into the vtmp slots
for the access micros to look up, therefore **clobbered v1's live tag**
at every gather commit.

### Measured symptom (TSVC LARGE, s353)

gcc allocates v1 as chain 2's ip-slice data register, and gather 0
commits between chain 2's `vle32.v v1` and its `vsext.vf2 v6,v1`
(binary at `tsvc-gem5/bin/GNU/tsvc_vec_LARGE`, s353 loop at 1af56-1b084).
Chain 2's extend therefore anchored to *chain 0's* vsll every
iteration:

- `linksFormed` = 6,403 instead of 5 — gathers 0 and 2 ping-ponging
  head H0's single consumer slot (`consumers_per_producer=1`);
- `chainsRejected` = 12,796 ≈ 1/iteration — chain 2's walked chain
  became ext,sll,ext,sll: not collapsible;
- `formsAdopted` = 3 of 5 — chain 2's own head never received a
  consumer, so **one of the five chains produced no indirect
  prefetches at all** (real s353 coverage loss, not a stat artifact).

s4112/s4113/s4115 were immune (linksFormed = 1): their codegen keeps no
live index provenance in v1 across a gather's commit window.

### The fix

Three coordinated changes, no behavioral knob (this is a model bug):

1. `src/cpu/vector_chain_table.hh`: the PT grows from 32 to **40
   slots** — the architectural vregs plus the vtmps.
2. `src/cpu/vector_chain_table.cc`: every `& 31` mask dropped
   (destination collection, gather source lookup, `VCpyVs`
   propagation, arithmetic source classification), with bounds guards
   instead of masking.
3. `src/arch/riscv/insts/vector.hh` (`VlIndexMicroInst::vecMemInfo`,
   fork-added code): the micro's `vs2RegIdx` is *relative* (which vtmp
   copy of the index group — see `VlIndexMicroConstructor`,
   `src/arch/riscv/isa/templates/vector_mem.isa:1029`), so it now
   reports the absolute index `VecMemInternalReg0 + vs2RegIdx`,
   keying the same slots `VCpyVs` writes.

A second latent corruption disappears with the same change: strided /
whole-register / segment micros whose *destinations* are internal
registers previously invalidated `pt[0..7]` (architectural v0-v7's
tags) on every commit; they now invalidate the vtmp slots they
actually write.

### Expected effect

On s353: `linksFormed` → ~5, `chainsRejected` → ~0, `formsAdopted` →
5, and chain 2 starts contributing indirect prefetches (coverage
should rise). All other TSVC kernels bit-identical in table behavior
(their PT slots 0/1 carried no live tags at gather commits). Any past
s353 chain-table numbers (links/rejects/adoptions) predate this fix
and are not comparable.

**Validated 2026-08-19** (MEDIUM, center params, `validation_aug19/V1`):
s353 linksFormed **5** (pre-fix churn ~2/iteration), chainsRejected
**0**, formsAdopted **5** (was 3), and all four kernels' checksums
match the pre-fix goldens (functional behavior untouched — the fix is
observability-plumbing only).


## Ara-shaped cache hierarchy (`--ara-cache`, 2026-08-19)

A third hierarchy option beside the split (`--vector-cache`) and unified
(`--unified-cache`) chains: **the L1D is scalar-only and the L2 is
shared** — vector accesses bypass the L1 entirely, the shape of the real
Ara SoC (CVA6's private L1D; the VLSU talks straight to the shared
L2/AXI).

### Topology (per core)

```
             core dcache_port
                    |
              VectorSplitter          (src/mem/vector_splitter.cc, reused unchanged)
              /            \
      l1d-cache-N        vector-buffer-N   (tiny 1-cycle "vbuf")
              \              /
               L2XBar ———————
                    |
                l2-cache-N            (shared)
                    |
                  membus
```

L1I and the page-table walker stay on the scalar path, as in the stdlib
parent. Coherence is ordinary snooping on the per-core coherent L2XBar:
a vector read of a line dirty in the scalar L1D gets the snooped data, a
vector store invalidates the L1D copy, and the splitter forwards snoops
from either mem-side port up to the CPU for LSQ ordering.

### The vector buffer (vbuf)

`rvv/ara_cache_hierarchy.py` inserts a tiny cache (default **512B / 8
lines, 1-cycle, 16 MSHRs**) between the splitter's vector port and the
crossbar, for two reasons:

1. **Protocol (discovered by crash, not foreseen in the design).** The
   O3 LSQ pointer-matches its responses
   (`LSQ::SplitDataRequest::recvTimingResp`, `src/cpu/o3/lsq.cc:1242`
   asserts the packet is one of `_packets`), but a cache supplying
   dirty data to a snooped request responds with a **copy** of the
   packet (`Cache::doTimingSupplyResponse`). A cacheless LSQ port
   directly on a snooping crossbar therefore dies on the first vector
   read of scalar-written data — i.e., on every workload with a scalar
   init phase. (Connecting the raw port also panics the stock
   `SnoopFilter`: "missing the original request",
   `src/mem/snoop_filter.cc:278`.) gem5's classic memory system
   requires the direct peer in such a topology to be a cache — the
   same reason coherent DMA gets an `iocache`
   (`_setup_io_cache`). The vbuf is that cache.
2. **Modeling.** It stands in for the VLSU's request buffering: its
   MSHR count (`--ara-vbuf-mshrs`, default 16) bounds the vector
   unit's outstanding memory transactions (the AXI depth), and its few
   lines give the coalescing window real VLSUs have.

Measured on the gdp_test smoke run: vbuf demand miss rate **99.7%**
(276,750 of 277,527) — at 8 lines it is not acting as a data cache;
every vector access still pays the L2 path.

### CLI

```
--ara-cache                      select the hierarchy (excl. with
                                 --vector-cache / --unified-cache)
--ara-vbuf-size    (512B)        vbuf capacity
--ara-vbuf-mshrs   (16)          VLSU outstanding-transaction bound
--l1d / --l2, --l1d-mshrs / --l2-mshrs   as usual
--prefetcher X --prefetcher-level l2     vector-side prefetchers MUST
                                 attach at the shared L2 (the L1D never
                                 sees a vector access; l1 is rejected
                                 with an explanatory error).
--scalar-prefetcher              may attach to the scalar L1D alongside.
--prefetcher-side                ignored (one chain).
--l2-prefetcher / --stream-demote   rejected (unsupported here).
```

The sideband wiring (VectorChainTable / TycheChainTable /
RevelaStreamTable, MMU registration) is identical to the sibling
hierarchies.

### Validation (2026-08-19, `validation_aug19/A1..A4`)

- **A1** (gdp_test, base): checksum PASS; splitter routing exact
  (scalarReqs == L1D demandAccesses = 1,083,778; vectorReqs == vbuf
  demandAccesses = 277,527); conflictStalls active (15).
- **guard**: `--prefetcher vtyche --prefetcher-level l1` rejected with
  the explanatory error.
- **A2** (gdp_test, vtyche@L2): full pipeline wired from the L2 vantage
  — chain-table link formed, form adopted, chunk events observed,
  stream candidates issued. (gdp_test is too small/warm to exercise
  conversion; see A3/A4.)
- **A3/A4** (TSVC MEDIUM L8W32, base / vtyche@L2 with pipelines=5 +
  capture_on_read_hit): **all 8 checksums match the goldens.** vtyche
  trains fully from the L2 vantage — s353 5 links / 5 forms (the PT
  fix visible here too), ~16,000 index-line fills per single-chain
  kernel (= every ip[] line), full conversion. Speedups over the ara
  base: s353 1.31x, s4112 1.42x, s4113 1.21x, s4115 1.40x, coverage
  0.96-1.00 on three of four kernels — larger relative wins than on
  the sibling hierarchies because the ara base pays L2 latency on
  every vector access, which is exactly the hardware story this
  topology is for.

### Comparability

Every vector access pays the L2 path in this hierarchy, so cycles are a
**new baseline era**: not comparable to split- or unified-hierarchy
results.


## VTyche: batch-granular residency feedback (`drop_batch_confidence`, 2026-08-19)

`--pf-param drop_batch_confidence=N` turns the cache's own drop verdict
into a residency oracle: when the cache drops one of vtyche's targets
because the line is already resident (`pfHitInCache`), the drop is
attributed to the **batch** that generated it, and after N drops from
the same batch, the batch's remaining targets are forfeited. Default 0
= disabled, bit-identical to before.

### What a batch is

The set of indirect targets converted from ONE captured index line (one
64B line of the index array = `blkSize/EEW` raw indices, 16 for int32).
It is already the design's working unit — the slice buffer holds
`CapturedLine`s, conversion drains one per event, `targetsDeduplicated`
counts "duplicate lines within one batch" — and it is the granularity
at which residency was measured on TSVC: 16.00 distinct target lines
per ip[] source line, resident **all-or-nothing** (87.5% of source
lines have a target set identical to the line two batches back). One
drop therefore predicts the sibling targets are resident too: one
probe pays for up to 15 kills. This is the lightweight version of the
residency-registry designs from the earlier pfHitInCache analysis - the
cache tracks residency, the prefetcher just samples its verdicts.

### What the existing method did

Nothing could react to drops: `Base::pfHitInCache()`
(`src/mem/cache/prefetch/base.hh`) was a non-virtual stat bump, so the
prefetcher never learned that its last packet died at the residency
check (`BaseCache` drop path, `src/mem/cache/base.cc:955-975`). Post-rde
the resident-drop population is the accuracy limiter wherever
prefetches are timely (memory-side accuracy 0.88-1.00): the ip[] reuse
structure at L8W32 and s353's cross-producer duplication.

### What this fork adds

1. **Core hook** (the one core-file change): `pfHitInCache()` is now
   `virtual`, default body unchanged. The cache calls it synchronously
   right after `getPacket()` returned the packet, so the prefetcher can
   attribute the drop to the exact address it just issued.
2. **Batch tagging** (`vector_tyche.cc`): each emitted target line is
   tagged with its source index line's VA (`emittedFrom`, bounded FIFO
   of 2x queue_size). The existing `getPacket()` peek consumes the tag
   into a one-shot latch. Stream candidates carry no tag and are
   invisible to the feedback — the stream half is never touched.
3. **The override**: on an attributed drop, tally it against the batch
   (`batchDropSamples`; small recent-batch list). At the
   `drop_batch_confidence` threshold (`batchesAborted`):
   - the batch's targets still staged in `pfq` are removed (same
     idiom as `pfRemovedDemand`);
   - the batch's not-yet-emitted remainder is suppressed at the
     emission push site (bounded `abortedBatches` FIFO, 8 entries —
     an aged-out abort lets a genuine later revisit re-emit).
   Both count in `targetsFlushedOnDrop`.

Known leak, by design: targets already parked in
`pfqMissingTranslation` cannot be removed (`translationComplete`
asserts its entry is still listed), so they issue and get dropped by
the cache as before, bounded by the translation-queue depth.

### The confidence knob

`drop_batch_confidence=1` aborts on the first drop — correct when
residency is all-or-nothing per batch (the TSVC-measured case).
Higher values demand more evidence before sacrificing a batch, for
workloads where batches are only partially resident (first-drop-abort
would forfeit the non-resident remainder there). `pfHitInMSHR` drops
never count: in-flight is not resident, and those siblings are still
worth issuing.

### Expected effect (not yet run)

s4112 sd16_l2_m16_L8W32 arithmetic predicted: issued 583K -> ~275K,
accuracy 0.43 -> ~0.9 at zero coverage cost.

### Measured (2026-08-19, s8 sweep + s13 spmv; controls = s7 rde rows)

Validation (MEDIUM s4112, dbc=1): drops -72%, accuracy 0.31 -> 0.59,
pfUseful -2.1%, checksum golden - mechanism confirmed.

Full sweep, dbc in {1,2} on the 8 TSVC LARGE winner cells (split
local/kazi, bit-exact parity verified) and poisson3Db jds at the
vector_indirect best cell:

- **Accuracy always improves**, dramatically where the resident-drop
  population was the limiter: sd16_l2 L8W32 0.38-0.44 -> 0.78-0.81
  (dbc1); the sd32_l1 cells the retired tail aged out of reach now
  respond too (0.25 -> 0.52).
- **Cycles never improve.** TSVC geomean vs the rde controls: dbc1
  1.114, dbc2 1.055. l1-attach cells ~neutral (1.00-1.05); the
  damage concentrates where the all-or-nothing premise FAILS:
  TSVC L1W1 l2-attach (random per-line ip[], scattered residency)
  dbc1 = +40-72% cycles, coverage 0.95 -> 0.56; spmv poisson3Db
  (28.6/32 unique lines per chunk) dbc0 2.053x -> dbc1 1.534x ->
  dbc2 1.758x, coverage 0.94 -> 0.71 -> 0.85.
- **dbc2 dominates dbc1 everywhere on cycles but never beats dbc0.**
  At L8W32 l2-attach dbc2 is nearly free (cycles +0.4-1.7%, coverage
  0.99) while accuracy still roughly doubles (0.38 -> 0.70-0.74).

Interpretation: in gem5's timing model drops cost nothing, so the ONLY
cycle-relevant effect of the feature is the coverage it sacrifices -
any collateral at all is a net loss. The batch-coherent-residency
premise holds on TSVC's L8W32 block-reuse pattern and nowhere else
tested. **Keep the default 0 for performance runs**; the knob's value
is traffic/accuracy shaping (issued -33% at zero coverage cost in its
best cell), which matters for modeled bandwidth/energy, not sim
cycles. Rows appended to tsvc_large.xlsx vtyche sheet
(axis=drop_batch_confidence, s8_vtyche_dbc*); spmv runs in
vector_indirect_results/runs/s13_p3db_*.

### Fractional batch discard (`drop_batch_fraction`, 2026-08-21)

`--pf-param drop_batch_fraction=F` (float in (0, 1], default 1.0 =
bit-identical whole-remainder abort) softens the confidence verdict:
once a batch crosses `drop_batch_confidence`, only fraction F of its
REMAINING targets is discarded instead of all of them.

Motivation: the s8/s13 sweep showed the all-or-nothing abort is only
correct where residency is batch-coherent (TSVC L8W32); on scattered
residency (poisson3Db, 28.6/32 unique lines per chunk) every forfeited
sibling was a needed one (coverage 0.94 -> 0.71 at dbc1). F gives the
knob a middle ground: discard in proportion to the evidence rather
than betting the whole batch on one probe.

Mechanism: `abortedBatches` entries now carry a Bresenham accumulator
(`acc += F; drop when acc >= 1, acc -= 1`), shared between the
abort-time prefetch-queue flush and the push-site suppression in
`drainEmission`, so exactly fraction F of the batch's remaining
targets is dropped, spread evenly (F=0.5 drops every second target,
F=0.25 every fourth). A kept staged target keeps its `emittedFrom`
batch tag, so if the cache later drops it as resident too, the drop
re-tallies (`batchDropRecs` re-arms after each abort) and thinning
continues — sustained evidence keeps thinning; a wrong verdict costs
only F of the remainder, and the survivors' outcomes (issued and
missed vs dropped resident) are the feedback that re-arms or ages out
the abort. Only read when `drop_batch_confidence != 0`; validated
(0,1] in the constructor. Measured 2026-08-21 (dbf_sweep): F is a pure
interpolator between dbc-on and dbc-off — linear in useful/issued,
monotone in cycles, never sign-changing. Optimum stays at the corners;
motivated the spatial regrouping below.

## VTyche: residency-interval registers (`residency_intervals`, 2026-08-21)

`--pf-param residency_intervals=N` (default 0 = disabled, bit-neutral;
companions `residency_conf_threshold`, default 2, and
`residency_gap_lines`, default 2) replaces batch-granular residency
inference with a spatial model trained by the cache's own verdicts.

### Why (what the dbc/F sweep proved)

drop_batch_confidence generalized one resident-drop across its BATCH —
a temporal grouping. The sweep showed that is the wrong unit: on
scattered index streams (poisson3Db, TSVC L1W1) batch-mates of a
resident line are usually NOT resident, so whole-batch (or fractional)
forfeit always trades coverage for traffic. Residency is instead
contiguous in the TARGET ARRAY's own coordinates (the hot band of x[]
that mesh-neighbor rows keep re-referencing; TSVC's reuse blocks; hub
lines) — so the right generalization of a drop is spatial, not
temporal.

### What the existing method did

Nothing tracked the resident portion of gather target arrays. The
dedup window (`dedup_buffer_size`) remembers recent EMISSIONS — a
traffic window that is structurally blind to the 4-50-chunk gap band
(emitted too long ago for the window, touched recently enough to be
resident), which is the ~20% pfHitInCache population on spmv
poisson3Db.

### What this fork adds (all in vector_tyche.cc/.hh)

N registers, each a block-aligned VA span `[lo, hi)` + 3-bit
confidence:

- **Train on drops**: `pfHitInCache()` (the existing virtual hook) now
  also latches the dropped packet's TARGET line (`lastIssuedTarget`,
  set in the `getPacket()` peek from the batch tag — streams carry no
  tag and stay invisible). A drop within `residency_gap_lines` of an
  interval extends it and bumps confidence (overlapping registers
  merge); an isolated drop allocates over the least-confident
  register at confidence 1.
- **Suppress at conversion**: `drainEmission` forfeits a target inside
  an interval at `residency_conf_threshold` before it costs a queue
  slot (`targetsResidencySuppressed`).
- **Trim on misses**: `notify()` checks observed demand MISSES against
  the intervals; a miss inside one trims it (larger half survives) and
  decays confidence (`residencyIntervalTrims`). The trim is the ONLY
  aging mechanism — no epoch clock, no window: the model un-learns a
  stale span at the cost of exactly one demand miss.

Safety property: suppression requires spatially CLUSTERED drops
(confidence only grows on extension), so scattered residency leaves
the mechanism inert — the dbc failure mode cannot engage. Storage is
dataset-size invariant (spans are bounded by cache residency): 8
registers ≈ 8 x (2 x 8B + 1B) ≈ 136B. ROI stats-reset clears the
registers with the rest of the learned state (`resetLearnedState`).

Stats: `residencyDropsTrained`, `residencyIntervalAllocs`,
`residencyIntervalTrims`, `targetsResidencySuppressed`.


## SpGEMM benchmark — workspace gather/scatter kernel (2026-08-19)

New RiVec-style benchmark at
`~/riscv-vectorized-benchmark-suite/_spgemm/`: **C = A·A** on a CSR
matrix using the dense-row-workspace ("linear combination of rows")
formulation of Kjolstad et al., *Tensor Algebra Compilation with
Workspaces*, Fig 1(d) (`gem5/spmm.pdf`). Built to stress indirect
prefetching with patterns spmv does not have.

### Structure (mirrors _spmv conventions)

- **Input**: the existing binary `.csr` images (`_spmv/input/*.csr`,
  same magic/loader). NOTE DISCOVERED HERE: those images store
  **1-based column indices** (mtx2csr copies the MatrixMarket index
  verbatim; _spmv tolerates it because `x[ja]` is only ever an
  off-by-one heap READ). spgemm uses `ja` as a row selector too, so
  row lookups use `ja-1` while the workspace/marker arrays are sized
  N+1 and indexed with the raw value — keeping the vector gather chain
  free of index arithmetic.
- **Phases**: binary load -> scalar symbolic phase (marker array,
  pre-ROI, as the paper's pre-assembled output assumes) -> numeric ROI
  (vector then serial, one stats dump each) -> verify.
- **CLI**: `spgemm_*.exe <file.csr> [nrows] [file.verif] [dump=path]`.
  `nrows` bounds the output rows (gem5 sim-time control; goldens are
  per (matrix, nrows)). `dump=` writes a **binary** golden (magic +
  count + doubles — text at nnz(C) millions would drown gem5-SE in
  fscanf instructions).
- **Verification**: vector vs serial in-binary AND serial vs golden,
  relative tolerance 1e-6 (vfmacc's fused rounding vs the host
  golden's `-ffp-contract=off` mul+add). Prints "Verification pass"
  (sweep-marker compatible) and a checksum.
- **Build**: `Makefile.gcc` — host golden build (native gcc) +
  riscv serial/vector with the suite's canonical GNU flags.

### The kernel's prefetch-relevant shape

Per output row: (1) *scale-accumulate* — for each nonzero of A's row
i, strip-mine row k of A: unit-stride `vle64` of ja -> `vsll 3` ->
`vloxei64` gather of the workspace -> `vfmacc` -> `vsoxei64` scatter
back (legal without conflict handling: column indices within one CSR
row are unique). (2) *compress* — unit-stride `vle64` of the output's
ocrd -> gather workspace -> unit-stride store + scatter zeros. Two
distinct index->gather chains per row plus an indexed-store stream;
the workspace (85,624 doubles for poisson3Db = 685KB) exceeds the
512KB L2. vsetvl announces row-length extents (much shorter than
TSVC's whole-array announcements).

### Validation (2026-08-19)

- Host self-verify: football (full) and poisson3Db (R=4096, 7.2M FMAs,
  nnz(C)=5.9M) both pass.
- gem5 (`validation_aug19/G1/G2`): football serial and vector builds —
  in-binary vector-vs-serial cross-check and golden compare all pass;
  first exercised use of `vsoxei64` scatters in this fork's pipeline.
- gem5 poisson3Db R=4096, base + vtyche-best (G3/G4): **both verify**
  (in-binary vector-vs-serial cross-check + binary golden; identical
  checksums across configs). The prefetch result is the designed
  discriminator working: at the exact config that wins **2.053x on
  spmv with the same matrix**, vtyche gets only **1.088x** on spgemm
  (vector ROI 190.7M -> 175.3M cycles), coverage 0.57 vs 0.94,
  accuracy 0.22 vs 0.78, elementsConverted 4.6x the needed FMAs,
  stalenessAborts 343K. Mechanism: the index-stream producer now walks
  ~3.5-line row segments whose start addresses are the SECOND level of
  indirection (ia[ja[pB]]) — after every segment the walk extrapolates
  16 lines in MEMORY order while the traversal jumps in DATA order, so
  the stream half restarts constantly (3.27M chunk events) and
  converts mostly-mistimed lines. The missing capability is chasing
  ja[pB] -> ia[k] -> ja[ia[k]..] (DROPLET-style level-2 prefetch);
  this kernel quantifies its worth at ~1.9x of the 2.05x spmv win.


## VTyche: VLEN-pinned stream frontier (`prefetch_distance`, 2026-08-21)

### What gem5/VTyche did before

VTyche's stream walker (`streamAhead`,
`src/mem/cache/prefetch/vector_tyche.cc`) denominated its lookahead in
chunks of the **observed access size**: the walk window was
`[addr + size, addr + size*(streaming_distance+1))`, where `size` is
that chunk's `vl*sew` bytes from the demand access. For full-vl
unit-stride loads this already scales with VLEN automatically, but on
partial-vl chunks (short spmv rows, loop tails, flat-kernel remainders)
the byte lookahead shrinks proportionally with `vl` — the long-known
"size-scaled lookahead → late prefetch" failure. The `Prefetcher.py`
docstring also mis-stated the unit as "lines"; it has been corrected to
"chunks of the observed access size".

### What was added

A second, mutually exclusive distance knob on `VectorTychePrefetcher`
(`src/mem/cache/prefetch/Prefetcher.py`):

- `prefetch_distance` (default 0 = off, bit-neutral): frontier distance
  in **whole-VLEN chunks** (`vlen/8` bytes each). When set, each chunk
  event walks `[addr + N*vlen/8, addr + size + N*vlen/8)` — the frontier
  is pinned a fixed byte distance ahead of the demand chunk's start,
  independent of the observed `vl`, and the window is as wide as the
  demand advance so consecutive windows tile without gaps even when
  `size != vlen/8` (LMUL > 1). Steady-state degree equals the demand
  advance: `ceil(VLEN/512)` cache lines per event for full-vl chunks.
- `vlen` (bits): wired automatically from `--vlen` by
  `rvv/riscv-rvv-se-ara-prefetcher.py` (injected into the `--pf-param`
  dict before the factory build; an explicit `--pf-param vlen=N`
  overrides). `fatal_if` rejects `prefetch_distance > 0` with no VLEN.

Semantics that fall out of the fixed frontier: a fresh or reset window
starts **at** the frontier (no burst-fill of the near window), so
`stream_start_at_distance` is implied and skipped in this mode, and
`streaming_distance` is ignored. The high-water dedup (`limitAddr`),
backward-jump restart, and `limit_gate` announced-extent clamp are all
unchanged and apply to both modes.

Usage: `--prefetcher vtyche --pf-param prefetch_distance=8` (no new CLI
flag; VLEN wiring is automatic). Caveat for sweeps: the indirect half's
timeliness rides on the stream's fills, so distances tuned for
`streaming_distance` do not transfer 1:1 to `prefetch_distance` on
partial-vl workloads.

## VTyche renamed to VIPER (2026-08-21)

The vtyche prefetcher is now officially named **VIPER** (Vector Indirect
PrEfetcheR) for publication. Mechanical rename, no behavior change:

- CLI: `--prefetcher viper` (clean break: `vtyche` is no longer
  accepted; old sweep scripts fail loudly).
- Files: `src/mem/cache/prefetch/vector_tyche.{cc,hh}` →
  `viper.{cc,hh}`.
- C++: class `VectorTyche` → `Viper`, `VTycheStats` → `ViperStats`;
  debug flag `Viper` (new; `VTyche` kept for vtyche2).
- Python: SimObject `VectorTychePrefetcher` → `ViperPrefetcher`
  (`Prefetcher.py`), factory key, hierarchy/config prose updated.

**Not renamed:** `vtyche2` (`VectorTyche2`) and `vhybrid` keep their
names, as do all historical sections of this document and archived
sweep scripts/results — pre-rename stats and configs refer to vtyche.
Per-stat names inside the prefetcher are unchanged, so stats parsing is
unaffected; only script-side `--prefetcher` strings needed updating.

## VIPER: random tail-drop throttle (`random_drop_interval`, 2026-08-23)

A randomized prefetch-throttling experiment: instead of thinning
converted targets at a fixed fraction (`drop_batch_fraction`, which the
2026-08-21 dbf sweep showed to be a pure interpolator between the 0%
and 100% corners), this mechanism drops a **random proportion of each
captured index line's tail**, with the drop intensity redrawn at random
**epochs**. Tail policy = the dropped elements are the line's *last* N,
so each draw is a randomized per-gather prefetch-degree clamp (changes
lookahead structure, not just rate — the axis the 2026-08-13 stride
sweeps showed to be the sensitive one).

### What the existing method did

VIPER's conversion (`convertChunk`, `src/mem/cache/prefetch/viper.cc`)
pushes every element of a captured index line through the shift-and-add
lanes; the only element-level suppressions were deterministic:
`limit_gate`'s announced-extent byte clamp (`cap.validBytes`), the
dedup seen-sets, and the canonical-address filter. gem5's stock
throttling knobs (`Queued`'s `queue_size`, `drop_on_full`) act on the
queue, not on what gets generated.

### What this fork adds (all in `viper.cc/.hh`, default bit-neutral)

Three params on `ViperPrefetcher` (`src/mem/cache/prefetch/Prefetcher.py`):

- `random_drop_interval` (default 0 = off): epoch length in observed
  demand accesses. The counter ticks in `Viper::notify()`; every
  interval it redraws `currentDropScale` uniformly from the rate set.
  Demand-paced on purpose — a cycle clock would stretch epochs under
  exactly the congestion being probed. The first epoch is always
  drop-free (`currentDropScale` starts at 0).
- `random_drop_rates` (default `[0.0, 1.0]`): candidate scales, each
  in [0, 1]. `{0, 1}` = on/off duty cycling; a singleton holds one
  scale forever. Passed through `--pf-param` as a comma list
  (`random_drop_rates=0,0.5,1`); `rvv/prefetcher_factory.py`'s
  `_coerce` learned to split comma values into lists for
  `VectorParam.*` params.
- `random_drop_geometric` (default false, added same day): randomize
  the epoch length itself — each observed access redraws with
  probability `1/interval` instead of counting to a fixed N, giving
  memoryless (geometric) epoch lengths with the same mean. Breaks
  phase-locking between the epoch clock and strip-mined loop
  periodicity (a fixed-N clock can land the same scale on the same
  program phase every strip). False = the fixed counter, bit-neutral.
- `random_drop_min_interval` (default 0, added same day): epoch-length
  floor for geometric mode — after a redraw, Bernoulli rolls resume
  only once the epoch is this many accesses old, so epoch lengths are
  min + Geometric(1/interval), mean min + interval (shifted
  geometric). Excises the geometric's short-epoch mass (drop epochs
  too brief to affect more than a couple of captured lines — pure
  coverage loss) while keeping the memoryless anti-phase-lock tail.
  Ignored in fixed mode; 0 = the pure geometric, bit-neutral. The
  `epochRedraws` stat (added with it) counts scale redraws, so the
  realized mean epoch length is observable (notifies/epochRedraws).
- `random_drop_seed` (default 1): seeds a private `std::mt19937`
  (`randomDropRng`), so the draw sequence is independent of everything
  else in the simulation and two configs differ only in this
  mechanism. Never wall-clock.

Per-line draw (tail policy), in `convertChunk` at the line's first
chunk (`cap.nextElem == 0`): while the epoch scale `s > 0`, draw
`tailDropElems = 1 + rng() % floor(elems * s)` (uniform in
`1..floor(elems*s)`) and store it on the `CapturedLine`. The effective
element count becomes `eff_elems = elems - tailDropElems`, composing
with the `limit_gate` clamp; the conversion loop, the done check, and
`conversion_lanes` resumption all run against `eff_elems`, so dropped
elements never consume a lane, a budget slot, or a queue entry.
Dropped elements are never recorded emitted (no seen-set or
dedup-window entry), so a later recapture of the line may still
convert them — same disposition as `drop_on_full`'s forfeits.

Stats: `linesRandomTruncated` (lines that drew a nonzero drop),
`elementsRandomDropped` (elements forfeited). Epoch state
(`notifiesSinceDraw`, `currentDropScale`) resets drop-free in
`resetLearnedState()` at each ROI stats reset; the RNG stream itself
continues (its position is not learned state).

### Semantics notes

- One draw per captured line, made *before* any element of that line
  converts — a line is never half-throttled mid-conversion, and a
  `conversion_lanes` split cannot change what gets dropped.
- Interpolator caveat: over many lines the *mean* dropped fraction
  under scale `s` is ~`s/2`; if swept results land on the dbf curve at
  the matching mean, that kills the throttling hypothesis (a useful
  outcome). The structural content vs dbf is (a) the drop is a
  contiguous tail (per-line degree clamp), (b) intensity varies at
  epoch granularity instead of holding a constant fraction.

### Validation (2026-08-23, `rvv/rand_drop_val/`)

Synthetic gather (`vector_indirect_eval.bin line_locality 65536 4`,
unified hierarchy, viper l1, sd16/sb8, m32):

- **Bit-neutral off**: pre-change build vs new build at default params —
  stats byte-identical except the two new counters (both 0).
- **Active smoke** (`random_drop_interval=64`,
  `random_drop_rates=0.25,1` — exercises the comma-list `--pf-param`
  path): benchmark still passes; `linesRandomTruncated` 8057 of 8076
  captured lines, `elementsRandomDropped` 24151, and
  `elementsConverted` fell 64584 → 40441 (Δ ≈ the dropped count, i.e.
  dropped elements really never reached a lane). `targetsGenerated`
  16130 → 13153, uncovered `demandMshrMisses` 1150 → 3762, cycles
  283k → 377k — the expected direction on a prefetch-bound kernel
  when ~all lines lose a tail (with the {0.25, 1} rate set nearly
  every post-first epoch is active).

Shifted geometric (same day): the min=0 repro matched the earlier
geometric run except the added `epochRedraws` stat lines, and
min_interval=256 cut redraws 1190 -> 239 (ratio 4.98 vs the predicted
(256+64)/64 = 5.0) with implied notify counts agreeing across both
runs — the epoch-length mean shifts exactly as designed.

Geometric mode (same day): re-running the fixed-mode active config on
the geometric-capable build reproduced the earlier active run
byte-identically (the mode branch does not perturb fixed mode's RNG
consumption), and the geometric run (`random_drop_geometric=true`,
same seed) passes with a distinct schedule of similar aggregate
intensity (8056 lines / 24117 elements dropped vs 8057 / 24151
fixed). Note the {0.25, 1} rate set keeps every epoch active in both
modes, so aggregates converge by design there; the modes separate
observably on rate sets containing 0 (duty cycling), where epoch
LENGTH decides how long drops stay off.

## Fix: vector-reduction micro-op dependency race (2026-08-24)

Root-caused and fixed the "vle32/e64m2 widening-chain corruption"
(found 2026-07-22, memory `gem5-vle32-e64m2-corruption`). The
corruption was never in the widening chain at all — isolation testing
(rvv/widen_chain_iso.c, 9 modes) showed the vle32/vsext front half and
the m2 indexed gather both clean, with corruption appearing only when
a REDUCTION consumed the gather.

### The bug (upstream-latent, O3-specific)

Vector reductions decompose into one micro per source register
(vfredusum m2 -> 2 micros). Each micro chained its running
accumulator through the PINNED, written-in-place destination
register: `tmp_val = (microIdx == 0) ? Vs1[0] : Vd[0]`
(templates/vector_arith.isa). A pinned in-place destination is
deliberately not a declared source, so the IQ had NO dependency
ordering the reduce micros: under the right wakeup timing (observed:
the v9-half's gather elements completing before the v8-half's), micro
i issued before micro i-1 and read the VPinVd tail fill (all-ones =
NaN) or a stale partial as its accumulator, and the last-writer-wins
in-place update hid the NaN. ExecAll/IQ traces show it exactly:
rvv/rand_drop_val/wdbg2 (redsum micro producing NaN from the pin
fill), wdbg4 (the v9-half waking and issuing one cycle before the
v8-half was ready). Timing/VLEN-dependent because it needs that
wakeup inversion; vfredosum (ordered) was protected only by a
per-micro IsSerializeAfter hack; vfredusum had nothing.

### The fix (isa/formats + isa/templates vector_arith.isa)

Reduction partials now chain through a NORMALLY RENAMED internal
register (`VecMemInternalReg0 + 1`, or `+2` when the vs2-overlap copy
occupies +1): every micro's partial creates a fresh rename version,
so micro i+1 carries a real IQ dependency on micro i. Only the LAST
micro writes vd (VPinVd count 2 -> 1 write). The micro constructors
take the actual micro count (`_numMicros`) for last-micro detection;
the acc read becomes unconditionally `Vs1[0]` (the declared source);
the IsSerializeAfter hack on ordered-reduction micros is removed (the
dataflow chain now enforces exact element order for both ordered and
unordered forms, cheaper than rename serialization). Applies to all
four reduce formats: int, float, and both widening variants.

### Validation (2026-08-24)

- rvv/widen_chain_test.c + widen_chain_iso.c (9 modes) + widen_dbg.c:
  all 12 previously-mixed runs now PASS at VLEN 512 (the
  deterministic failing config) and 2048.
- Full rvv-index-bench suite (11 benchmarks incl. heavy vfredosum
  use): all PASS with checksums bit-identical to the host goldens.
- Implication: TSVC-shaped autovec chains (vle32+vsext+vluxei64+
  redsum) are now trustworthy; the standing "re-verify TSVC checksums
  per config" caveat is lifted for this failure mode. vfredosum-heavy
  workloads get a timing improvement (serialization removed), so
  treat reduction-bound kernels as a minor new timing era.

## Chaining: pinned-destination write-ordering hazard (2026-08-24, PARTIAL)

While re-validating the rvv-index-bench suite at upstream element
widths, a SECOND corruption (distinct from the reduction race fixed
earlier the same day) was localized but NOT fully fixed.

Symptom: `sls` with int64 indices (`vluxei64` gathering f32) returns
NaN at large configs; `--disable-chaining` makes it PASS, so the fault
is in this fork's chaining machinery, not base gem5.

Evidence (ExecAll, rvv/rand_drop_val/sls_filtered.txt): mid-gather the
destination register's already-written elements 0-2 revert to the
`VPinVd` tail fill (0xff...), while element 3 writes normally — i.e. a
pin fill lands AFTER earlier element writes. Pinned registers are
deliberately NOT renamed (rename_map.cc:82-95), so every writer shares
one physical register and partial in-place element writes accumulate
in it; any out-of-order in-place write destroys them.

Shape sensitivity: `paged_attn` uses the identical `vluxei64`-of-f32
instruction and passes. The differentiator is that gcc allocated
sls's gather with DEST == INDEX register (`vluxei64 v2,(a2),v2`), the
overlap form whose macro-op emits the VCpyVs + VPinVd preamble. The
same overlap shape was flagged by isolation modes 3/5 of
rvv/widen_chain_iso.c.

Two robustness fixes were made (both validated, neither sufficient):
- `FUCompletion` now uses the chaining decision RECORDED AT ISSUE
  (threaded through `processFUCompletion`) instead of recomputing it.
  The recompute lacked the issue path's FU-pool-latency fallback and
  re-read vtype (which can change between issue and completion), so
  the two predicates could disagree and queue an instruction for
  functional execution twice.
- Instructions with a PINNED destination are excluded from chaining's
  early functional execution (`WakeDependents`), since early execution
  reorders in-place writes to a non-renamed register.

Regression status: the full 11-benchmark suite was re-run after both
changes — 10/11 PASS with checksums bit-identical to the host goldens
(all but `sls`). The residual sls failure is open; treat
dest-overlaps-index indexed loads under chaining as suspect until it
is root-caused.

## VIPER: eviction-clocked dedup (`evict_dedup_entries`, 2026-08-27)

### What the existing method did

VIPER's duplicate suppression is windowed in *emission order*: the
per-way cross-line window (`dedup_buffer_size`,
`src/mem/cache/prefetch/viper.cc` `drainEmission()`) compares each
target line against the lines emitted from the last N processed index
lines, and the within-line/vl-window seen-sets (`convertChunk()`)
cover one line or one gather chunk. Nothing tells any of them whether
a previously emitted line is *still in the cache*: a target recurring
beyond the window re-emits, the cache finds the line resident, and
gem5's queued prefetcher squashes it (`pfHitInCache`,
`src/mem/cache/prefetch/queued.cc`). The squash protects DRAM but the
conversion-lane slot, queue slot and translation slot are already
spent — measured as the pfHitInCache accuracy plateau: dedup windows
reach ~4 chunks while measured residency horizons run 50–800 chunks.

gem5 itself already delivers the two missing truth signals to every
prefetcher: `BaseCache::invalidateBlock` fires the "Data Update"
probe with an empty `newData` on every block departure
(`src/mem/cache/base.cc`), and `BasePrefetcher::regProbeListeners`
auto-connects a `PrefetchEvictListener` that forwards those to the
`notifyEvict()` virtual (`src/mem/cache/prefetch/base.cc`; no-op by
default, previously overridden in-tree only by SMS).

### What this fork adds (all in `viper.cc/.hh`, default bit-neutral)

`evict_dedup_entries` (default 0 = off) enables a **global LRU table
of emitted indirect target lines whose entries live until the cache
says the line is gone** — suppression clocked by residency instead of
by a fixed-depth window, so it holds at any reuse distance:

- **Insert** when a target is actually pushed to the queue
  (`drainEmission()`; drop_on_full overflow targets are not tracked —
  no prefetch went out).
- **Suppress** a recurring target whose line is tracked
  (`targetsEvictDeduplicated`), refreshing its LRU position
  (recurrence evidence).
- **Clear** on `notifyEvict()` — the proactive re-arm — and on an
  observed demand MISS to the line (`notify()`), the corrective
  re-arm that heals entries whose eviction was never matched (e.g. a
  prefetch dropped at translation). A demand HIT refreshes instead:
  LRU-by-access keeps entries for lines that are provably resident
  and recurring.
- **Replace** LRU-by-access at capacity (`evictDedupReplaced`).

VA/PA split: conversion, emission and the demand stream operate on
VAs (`use_virtual_addresses`), but the eviction probe reports PAs. The
`getPacket()` peek — already the one point holding a target's VA and
translated PA together (it does the same for stream-page
registration) — binds the PA into a side index that `notifyEvict()`
clears through.

Every approximation degrades toward emitting: a replaced or
never-bound entry costs one redundant emission that the cache
squashes exactly as before. The only fail-unsafe window is an
eviction landing between suppression and the demand re-touch, and the
miss-clear bounds that at one demand miss. The table can never
usefully exceed the attach cache's line count (512 at a 32 KiB L1);
`resetLearnedState()` wipes it at ROI stats resets like the rest of
the learned state. Orthogonal to and composable with
`dedup_buffer_size` (per-way, order-windowed) and the
residency-interval registers (span-granular, verdict-trained).

New stats: `targetsEvictDeduplicated`, `evictDedupEvictCleared`,
`evictDedupMissCleared`, `evictDedupReplaced`.

## VIPER: set-pressure gate (`set_pressure_threshold`, 2026-08-27)

### What the existing method did

Nothing in VIPER (or gem5's queued prefetcher) bounds how much of one
cache SET speculation may occupy. With 64 sets covering a 4 KiB
alias window, an indirect target stream spanning hundreds of KB maps
~hundreds of lines into every set per pass; prefetches landing in an
already-speculation-full set evict earlier not-yet-used prefetches
(self-thrash — measured elsewhere as sub-1% prefetch survival at L1
under deep distances). The cache's replacement policy sees no
difference between demand and speculative occupancy.

### What this fork adds (viper.cc/.hh, default bit-neutral)

`set_pressure_threshold` (default 0 = off) + `set_pressure_sets`
(default 64) add one saturating 3-bit counter per cache set holding
that set's **prefetch-origin occupancy**: +1 when a prefetch fill
lands in the set (`notifyFill`, `isHWPrefetch()`), −1 when a
prefetch-origin line leaves it (`notifyEvict`) — used or unused. At
emission (`drainEmission`), an indirect target whose destination
set's counter is at the threshold is dropped
(`targetsSetPressureDropped`), not recorded emitted, so a later
recapture re-emits once the set drains.

Membership ("was this line prefetch-filled?") is kept
prefetcher-side in a PA-line set rather than read from the cache's
pf-bit, because `BaseCache` clears that bit at the first demand use
(that is how pfUseful is counted) — an eviction of a *used*
prefetch-origin line reports `hwPrefetched = false` and would leak
every useful fill into a permanent +1. The hardware analogue of the
membership set is one extra tag bit per line (512 bits = 64 B) read
by the victim path; the counters are 64 × 3 bits = 24 B.

Set indexing is translation-invariant by construction:
`set_pressure_sets × blkSize` must not exceed the page size
(constructor `fatal_if`), so the index bits sit inside the page
offset and the VA-side emission check selects the same set as the
PA-side fill/evict accounting (64 × 64 B = exactly one 4 KiB page
for the 32 KiB / 8-way L1D).

Fail-safe direction throughout: counter saturation (clipped +1s,
`setPressureSaturated`) and the reset-time undercount (resident
prefetch-origin lines survive `resetLearnedState`, counters do not)
both under-throttle — extra emissions the cache squashes as before.
Composes with `evict_dedup_entries` (exact-line residency) — the two
suppress disjoint populations.

New stats: `targetsSetPressureDropped`, `setPressureSaturated`.

## Discovery fixes for natural-vectorization kernels (2026-08-27)

Two root causes made VIPER's indirect half completely silent on the
`rvv-index-bench` natural GAP variants (formsAdopted=0 on
bfs/cc_sv/sssp natural while the forced rewrites adopt and convert;
the naturals' entire viper win was the stream half):

### 1. Right shifts in the affine collapse (`viper.cc collapse()`)

Existing: the collapse accepted extend / vsll / pow2-vmul / vadd/vsub
only; `Srl`/`Sra` fell to the reject default, although the
VectorChainTable already records them as replayable transforms. The
natural direction-optimizing BFS gathers bitmap WORDS —
`vle32 -> vsrl.vi 5 -> vsll.vi 2 -> vluxei32` — so every chunk's chain
walked and was rejected (`chainsRejected` = 987K = every chunk).

Added: `LinearForm` gains `rshift`/`rshiftSigned`, applied between the
extension and the left shift (`applyForm`, `preLaneKey`) — the lane's
shifter becomes a funnel shifter, still one circuit. Accepted only
while nothing has scaled or biased the value (it commutes with
neither), and the shift kind must match the value's 64-bit image
(`Sra` iff sign-extended, `Srl` iff raw/zero-extended) so the 64-bit
replay stays exact; `SExt`/`WMul` order guards learned `rshift`.

### 2. vm-aware v0 source classification (`vector_chain_table.cc`)

Existing: the arithmetic source classifier skipped ANY vector source
with architectural index 0 as "the v0 mask". But v0 is only a mask as
the IMPLICIT vm=0 operand; as an explicit operand it is ordinary
data. GCC's register allocator cycles v0 through index chains in
mask-heavy loops — `gap_cc_sv_natural`'s whole chain is
`vle32.v v0 / vsll.vi v0,v0,2 / vluxei32.v v0,(a1),v0` — so the vsll's
only source was skipped, no transform link ever inserted
(`transformsInserted=0`), and the gather had nothing to walk
(`linksFormed=0`). Same shape explains sssp_natural's silence; the
head path (`pt[dests]` unconditional) and the gather path
(`vecMemInfo().srcVReg`) were already v0-clean.

Added: read the vm bit (encoding bit 25, the same raw-encoding
technique as the vtype extraction) — unmasked ops treat a v0 source
as data; masked ops keep the skip (the implicit mask; a masked op
whose data source is also v0 stays conservatively untracked).

### Use-decrement mode (`set_pressure_use_decrement`, 2026-08-27)

Validation of the pure-occupancy gate on spmv/poisson3Db (threshold
4, unified/L1/m16 viper center) showed the predicted failure: the
counters track the whole prefetch-origin working set, and on a
workload where prefetched lines ARE the working set every set
legitimately reads >= 4/8 in steady state — the gate dropped 1.65M
mostly-good targets, halved pfUseful, and cost +47% cycles.

`set_pressure_use_decrement=true` changes what the counters hold to
NOT-YET-USED speculation only: the first demand touch of a
prefetch-origin line (observed in `notify()`, PA side) retires it
from its counter and from the membership set — so its later eviction
is a no-op, and each line decrements exactly once, by whichever exit
comes first (use, or eviction-unused via `notifyEvict`). Healthy
prefetch pipelines then hover near zero pressure while genuine
dead-speculation pileups still accumulate. New stat:
`setPressureUseDecrements`.

## Chaining: pinned-destination write-ordering — ROOT CAUSE + FIX (2026-08-28)

Closes the residual from the 2026-08-24 PARTIAL entry above.

### Root cause

An indexed vector load macro emitted micros in the order
VCpyVs -> VPinVd -> element loads (`templates/vector_mem.isa`,
VlIndexMacroConstructor). The element micros source only rs1 + vtmp
(+ v0 when masked) — the pinned vd is NOT among their sources — so no
dependency orders the pin's whole-register fill before the element
writes; correctness rested on issue timing. Upstream's fixed 1-cycle
vector latencies always happened to satisfy it. This fork's ARA
latency model (SimdMiscOp -> dispatch floor + model latency) and
chaining's two-slot FU occupancy broke it: VCpyVs (older) issues,
the elements chase memory, and VPinVd — younger, starved behind the
busy AraSIMD pool — executes hundreds of cycles late, wiping the
elements already accumulated in the shared unrenamed physical
register (the sls ExecAll trace: elements 0-2 written, all-ones fill
lands between elements 2 and 3).

Corollary: the hazard never required dest==index aliasing — VPinVd is
emitted for EVERY indexed load. That is why `sssp_vector` (gather not
aliased) crashed alongside cc/cc_sv on amazon0302; aliasing only made
the losing timing more likely (sls vs paged_attn).

### Fix

Emit the VPinVd micros BEFORE the VCpyVs micros. The elements' only
gating producer (VCpyVs) is then YOUNGER than every pin and lives in
the SAME op class (SimdMiscOp), so the age-ordered ready queue cannot
issue it before the pins; a pin therefore completes no later than
VCpyVs and strictly before the first element write, which sits at
least a wakeup + issue + cache access behind VCpyVs. No dependency
edges, register counts, or latencies change — only intra-macro micro
order — so this is a (small) timing-era change for all vector-load
code, not a semantics change.

Residual, documented not fixed: a tail/mask-UNDISTURBED pin sources
the old vd and can still wait out VCpyVs (compilers and this suite
emit ta,ma); the segment-load template pins before its elements
already but its elements are cross-op-class and dependency-free, so
its ordering still rests on timing (segment ops are banned in the
suite for unrelated known bugs).

### Fix v2 (same day): issue-side gate instead of micro reorder

The micro reorder above was reverted: renaming the pin before the
index copies perturbed rename order into a SECOND latent pinning bug
— a squash leaves the pinned register's dependency-graph list and
pin counters unrestored, and the next producer's dispatch panics
("Dependency graph not empty", `addToProducers`) — the same
accounting family as the known narrowing-op panic. Rather than
repair upstream's pinned-squash bookkeeping, the ordering invariant
is enforced in the fork's own issue stage (`cpu/o3/inst_queue`):

- `pendingPinFill`: flat phys reg -> seqNum of the pin (a NON-memory
  writer of a pinned register at `addToProducers` — VPinVd) whose
  fill has not executed.
- `scheduleReadyInsts` refuses to issue a MEMORY micro with a pinned
  destination while an older pin's entry is present — deferred
  exactly like a busy FU, re-checked every cycle.
- The entry clears when the pin executes (`wakeDependents`) or is
  squashed (`doSquash`); `resetState` clears the map. Deadlock-free:
  the pin itself is never gated, and a squashed pin's elements are
  younger and squashed with it.

Arith writers of pinned registers (the narrowing shape) are
deliberately NOT gated — same-op-class age-ordered issue already
orders them, and that shape's separate accounting bug remains open
upstream. Micro order and rename behavior are untouched, so the
timing perturbation is confined to indexed/segment loads whose
elements would previously have raced their pin.

## VIPER: scalar level chain (`scalar_chain_enable`, 2026-08-28)

**What gem5/this fork had.** Viper's discovery is vector-only: the
VectorChainTable links a unit-stride producer vle to its dependent
gathers, and everything upstream of the vle's BASE register is
invisible. On row-hopping kernels (bfs/bc/sssp: `u = queue[i]` ->
`off[u]` -> `vle nbr+(off[u]<<2)` -> gather), the producer's base
jumps per row, rows are usually a single partial strip, and the stream
walk never locks on — the exact gap behind the 8/27 "stride >= viper
on GAP kernels" result. Separately, the TycheChainTable port
(`cpu/tyche_table.hh`, `--scalar-prefetcher tyche`) already learns
scalar dependency chains (IPT stride heads, per-register PT taint, DCT
links with constant-stability confidence) but replays them
per-instruction through an AGQ+ALU model, disconnected from viper.

**What was added.** The two are joined into a *level table* design:

- **Join + promotion (`cpu/tyche_table.{hh,cc}`).** At the O3 IQ's
  vector dispatch site (`cpu/o3/inst_queue.cc`, both insert paths),
  when a TycheChainTable is wired: every vector instruction taint-kills
  its integer destinations (`noteVectorWriter` — the vsetvl rule,
  also a latent-staleness fix for plain `--scalar-prefetcher tyche`),
  and a unit-stride vector load micro-op 0 records its base register's
  scalar provenance as a *join record* (`noteVectorBase`, which also
  runs the `formed` backward walk — the vle plays the dependent-load
  role of stock Tyche's formed marking). `scalarChainFor()` is the
  promotion walk: tail -> head via `lastDctPtr`, every link
  `needHandle()` (formed AND conf==3 AND dense), terminating at an
  IP-stride head, segmented at Load links into level descriptors
  {load PC, size/sign, recorded ALU ops to the next level's address,
  next load's imm}. Ops are COPIED out (a DCT clear cannot dangle
  them); `execOp()` is the stateless replay ALU. The both-tainted
  `row_end - e` sub feeds only vsetvl/branches, never the base chain,
  so chains compile around it by construction; sibling fan-out
  (`off[u+1]`) is invisible to the backward walk and never compiled.

- **Level Stream Table (`mem/cache/prefetch/viper.{hh,cc}`).** The
  per-producer stream tracking table is RENAMED `levelStreamTable`
  (LST, entries `LstEntry`) and generalized: scalar chain levels are
  entries in the same table, keyed by their load PC, holding the
  level's ops/element geometry and successor key instead of consumer
  ways. The IRT gains `{elemMask, ttl}` and routes captured lines to
  scalar levels exactly as it routes them to producers. Replay:
  a confident IPT trigger at a compiled head PC (scalar demand access
  — the IPT trains in `calculatePrefetch` on every observed read)
  issues the head line at `scalar_head_distance` strides ahead; each
  captured level line converts at the drain (one line/event, same
  lane budget) by reading its registered elements, replaying the ops,
  and issuing the next level's lines with their registrations riding
  along (getPacket is the VA/PA point, as for stream captures). The
  TERMINAL hop emits one line per value — the future producer-row
  BASE (Form B; the row tail is demand/announcement work) —
  registered under the producer PC so the unmodified vector pipeline
  converts its gather targets. TTL bounds each lineage's hops.

- **Resident capture (`mem/cache/{cache_probe_arg.hh,base.hh}`).**
  `CacheAccessor::readLine` (default false; BaseCache implements via
  `tags->findBlock`) lets pfHitInCache-dropped chain prefetches be
  read straight off the cache — one extra read-port access, stock
  Tyche's `mem_data.read`. Without it the chain starves on hot
  frontiers (the kernel just WROTE the queue lines, so head prefetches
  drop resident and produce no fill to capture).

- **Wiring.** `--pf-param scalar_chain_enable=true` (default off,
  bit-neutral: no calls, no table). Requires `--unified-cache` (the
  trigger is a scalar demand access) — enforced in
  `rvv/riscv-rvv-se-ara-prefetcher.py`; the factory
  (`needs_chain_table`) and `rvv/unified_cache_hierarchy.py` wire one
  TycheChainTable per core to both `cpu.tyche_table` and viper's
  `scalar_chain_table`. Knobs: `scalar_head_distance` (32),
  `scalar_conversion_cap` (16, the stale-frontier exposure cap on
  head lines), `scalar_max_levels` (4). Non-head levels with loads
  narrower than 4 bytes are rejected (the 16-bit element mask cannot
  express them).

**Verification signal.** Terminal outputs enter a 128-entry predicted-
base CAM checked against observed producer chunk starts:
`scalarBaseHits` / `scalarBaseMisses` is the chain's end-to-end
accuracy independent of timing. Discovery is fully counted
(`vectorBasesNoted`, `scalarChainsBuilt/scalarWalksFailed` table-side;
`scalarChainsAdopted`, `scalarHeadTriggers`, `scalarLinesConverted`,
`scalarLevelIssued`, `scalarResidentCaptures`, `scalarDroppedFull`,
`scalarStaleFlushes` prefetcher-side) so a silent non-discovery shows
in stats, not in a flat cycle count.

**Known limits (v1).** Strided producers (sssp's vlse pair) never
join — `vecMemInfo` classifies them None, so the kernel keeps failing
discovery at the same gate as before. Scalar head triggers exist only
where scalar demands reach the prefetcher (unified hierarchy, L1
attach). The announcement-tail half of Form B (extending the
demanded row to its vsetvl-announced extent) is NOT part of this
change.

## VIPER: shape-version chain invalidation (`scalar_shape_invalidate`, 2026-08-29)

**What this fork had.** The scalar level chain (`scalar_chain_enable`,
above) copies the TycheChainTable's chain OUT at adoption:
`Viper::adoptScalarChain` (`src/mem/cache/prefetch/viper.cc`) walks
the DCT once via `scalarChainFor()`, stores each level's recorded ALU
ops — register constants baked in as `ScalarOpRec::src` — plus the
next load's immediate and the head/successor PCs in `levelStreamTable`,
and memoizes the adoption on `TycheChainTable::generation()`
(`scalarAdoptGen`). The head trigger, the drain-side conversion and the
pending-capture path all gate on that same generation. The table
itself never stops tracking: a link's register constant is re-sampled
at every issue of its PC (`src/cpu/o3/inst_queue.cc`, the
`captureConstant` hook) and always stores the latest value
(`src/cpu/tyche_table.cc`, `captureConstant`); a re-observed link
refreshes its predecessor pointer (`lastDctPtr`, the link-refresh
branch of `dispatch()`); an IP-stride load re-asserts itself as head.
But `generation()` moves only in `clearDct()` — ROI reset or DCT
capacity overflow (`dctInsert`). So viper's copy was refreshed exactly
as often as the table filled up.

The 8/27 sweep's `--tyche-dct-entries` OFAT exposed this as a
correctness dependency on table capacity (bc_vector, sc center):

| DCT entries | clears | chains adopted | head triggers | elements converted | speedup |
|---|---|---|---|---|---|
| 24 | 9,973 | 8,417 | 445,991 | 10.8M | 1.066x |
| 48 (= 96, bit-identical) | 1 | 2 | 24,448 | 190K | 0.960x |

At 24 entries bc overflowed ~10K times in the ROI; every clear
invalidated the memo, the next producer chunk re-walked the by-then
current DCT (new phase's head load, new base constants) and viper
re-adopted. At 48 the table never filled, the two first-phase
adoptions ran the whole ROI with a head PC that was no longer the hot
loop's head, and `scalarBaseMisses` went 44K -> 397K. bfs (2 chains,
no phase change) was flat, confirming this is staleness, not size.

**What was added.**

- **`DctItem::exported` / `exportedPrev`** (`src/cpu/tyche_table.hh`):
  set on every link of a path by a successful `scalarChainFor()` walk
  — "a prefetcher holds a copy of this link" — together with the
  predecessor pointer the copy was compiled with (-1 for the head).
  Cleared with the link (clear / reset).
- **Two versions, two meanings** (`tyche_table.hh/.cc`). The table
  distinguishes a change that makes the copy *wrong* from one that
  makes a *different* chain live:
  - **`shapeVersion()`** — bumped by `noteShapeChange()` when an
    exported link changes what its replay would COMPUTE: a register
    constant re-sampled to a different value (`captureConstant`,
    `shapeBumpsConst` — the moment the copied-out `src` is wrong) or,
    for a link exported as a dependent, an operand-side /
    imm-vs-register / imm-value change in the link-refresh branch of
    `dispatch()` (`shapeBumpsOp`). **`chainVersion()`** is
    `generation() + shapeVersion()` — both monotone, each event moves
    exactly one — and it is what compiled levels are stamped with:
    a bump stops the head, flushes buffered lines, and forces
    re-adoption. Head<->dependent *relabels* (an inner load whose
    stride is intermittently confident is re-asserted as head by the
    IPT path and demoted again by the dependent path) change neither
    ops nor constants and do not count; unexported links never count.
  - **`routeVersion()`** — bumped by `noteRouteChange()` when the data
    flow AROUND a compiled chain moves: an exported dependent link is
    fed by a predecessor different from `exportedPrev`
    (`routeBumpsPred`), or a producer's join record moves to a
    different tail link / a new join appears (`routeBumpsJoin`,
    `noteVectorBase`). This only re-opens the promotion walk: viper's
    adoption memo is keyed on `scalarAdoptKey()` = chain key + route
    version, while the levels keep their chain-key stamp and stay
    live. The walk then decides — a different valid chain is compiled
    alongside the old one, an identical chain rewrites identical
    entries, a failed walk changes nothing.

  Why the split is necessary (measured, not theorised): the first two
  cuts treated a predecessor change as an invalidation. Per-site
  counters showed **every** bump on both kernels was `Pred` (bfs
  518K, bc 249K/307K; Const = Op = 0). On bfs the vle base is
  `nbr + e` (`rvv-index-bench/src/bfs_vector.c:136-145`) and `e` comes
  from the `off[u]` chain at a row start but from `e += vl` on every
  later strip: the same static `slli/add` link is legitimately fed by
  two dynamic paths, and the compiled row-start chain is exactly the
  one wanted (future rows; the strip tail is the announcement
  stream's job). Invalidating on each strip gated the head off
  two-thirds of the time (813K -> 272K triggers, 152K re-adoptions,
  766K failed walks because the strip path has no head): 1.214x ->
  1.192x on bfs, and bc's dct48 recovery stalled at 1.024x (0.96x
  without the flag, 1.066x at dct24) for the same reason. bc's
  phase change is the other face of the same event — a re-route to a
  different head through shared links, where the compiled chain is
  not wrong but silent — and that is precisely what a re-walk
  handles.
- **`scalar_shape_invalidate`** (`Prefetcher.py`, default off,
  bit-neutral): `Viper::scalarChainKey()` returns `chainVersion()`
  instead of `generation()`; it replaces every `generation()` compare
  on the scalar path (adoption memo, level stamp `LstEntry::scalarGen`,
  head trigger, `convertScalarLine`'s stale flush, pending-capture
  registration). Nothing else changes: a bump stops the stale head at
  its next trigger, flushes buffered level lines at the next drain
  (`scalarStaleFlushes`), and the next producer chunk re-walks and
  re-adopts. Re-adoption is refused while a changed constant is
  re-stabilising (`needHandle()` needs conf 3), which is the intended
  gap — no replay of a half-trained value.

**Why a counter, not a flag.** A dirty flag must be cleared by its
consumer, and the scalar path has several consumers reading at
different times (one memo entry per producer PC, one stamp per level
entry, checked at trigger / drain / capture). Clearing on the first
consumer would hide the change from the rest; a version number is a
flag that never needs clearing, and it is the idiom `generation()`
already uses, so viper gains no new code path. The alternative that
does work with a flag — a broadcast invalidate from the table into the
prefetcher — needs a table->prefetcher back-pointer that does not
exist (the prefetcher pulls through `scalarTbl`).

**Validation** (`rvv/sweeps/viper_sweep_827.py` legs `sc_si` /
`sc_si_dct48`, sheet `postfix`; earlier cuts kept in
`runs_postfix_si_v1|v2`). Flag off is bit-neutral (viper_sc_bfs_vector
rerun: ROI stats identical). Flag on, two-tier:

| cell | bfs speedup | bfs head trig | bc speedup | bc head trig | bc baseMiss |
|---|---|---|---|---|---|
| sc center dct24 | 1.214 | 813K | 1.066 | 446K | 44K |
| sc_si dct24 | 1.215 | 834K | 1.067 | 458K | 39K |
| sc dct48 (no flag) | 1.208 | 804K | 0.960 | 24K | 397K |
| sc_si dct48 | 1.209 | 824K | 1.032 | 276K | 173K |

Shape bumps are zero on both kernels (constants never change); route
bumps are ~one per non-first strip (bfs 522K, bc 250K/305K Pred) plus
one join move per clear, and cost nothing: zero stale flushes, the
head never stops. The flag is neutral where the capacity churn was
already keeping the chain fresh (dct24) and recovers most of the
dct48 collapse (0.96x -> 1.032x) without any clear.

**Residual gap (open).** bc at dct48+flag stays below dct24 (1.032x
vs 1.066x: half the head triggers, 4x the base misses). It is not an
exported-link change — the table inserts only 12 heads / 63 links in
the whole ROI at dct48 (vs 20.6K / 218K at dct24) and refreshes that
fixed set 13M times, so the promotion walk runs over long-lived links
whose predecessor pointers, `dense` flags and PT taint were last
refreshed in whichever phase last executed them, and some walks
compile a chain through a head that no longer runs (the chain is
silent, not wrong: no Const/Op bump can see it). Clear-on-full fixed
that by forgetting everything; the version counters by design only
notice changes to links a copy was made from. Closing it needs link
liveness — age out links not re-observed within a head window, or
have the walk reject heads whose IPT stride has not fired recently —
not more versioning.

## TycheChainTable: link liveness (`live_window` / `--tyche-live-window`, 2026-08-29)

**What this fork had.** The DCT (`src/cpu/tyche_table.hh`) only ever
forgets a link wholesale: `clearDct()` on capacity overflow or ROI
reset. A link that stopped executing keeps its predecessor pointer,
its `formed`/`dense` flags and — through the Propagation Table — the
taint on any register it wrote that was never rewritten. At a table
that never fills (`--tyche-dct-entries 48` on bc_vector: 12 heads /
63 links inserted in the whole ROI, refreshed 13M times) that state
belongs to whichever phase last ran the link, and three things go
wrong for the phase that is running now: an instruction reading a
register tainted by a dead link is classified both-dependent
(`doubleDepends` 1.09M -> 1.13M) and never becomes a link; a join
tail can name a dead link; and the promotion walk happily compiles a
chain up to a head that no longer runs (silent, not wrong — nothing a
shape or route version can see, since nothing on the copy *changed*).
Clear-on-full fixed all three by accident; the two-tier invalidation
above left bc at dct48 on 1.032x against 1.066x at dct24.

**What was added — a liveness clock, no replacement policy.**
`seq` counts DCT-matching dispatches (one increment at the top of
`dispatch()` where `denseCount` already found the link), each link
stamps `lastSeen = seq` when dispatched or inserted, and
`linkLive(idx)` is `seq - lastSeen <= live_window`. A dead link keeps
its state (a re-observation revives it with everything intact) but
is invisible at the three places above:

- `dispatch()` taint evaluation: a source register whose PT entry
  points at a dead link reads as untracked (`taintsDead`), so the
  instruction becomes a normal link with a register constant instead
  of a both-dependent chain break;
- `noteVectorBase()`: a dead tail is `vectorBasesUntracked`;
- `scalarChainFor()`: any dead link on the path fails the walk
  (`scalarWalksDead`, also counted in `scalarWalksFailed`).

`live_window` (TycheChainTable param, `--tyche-live-window`, default
0 = off, bit-neutral: every test short-circuits) is in DCT-matching
dispatches, so it scales with how much chain code is executing rather
than with wall time; 4096 is the validation value (a row of bfs/bc
touches ~10-30 links per strip, a phase is thousands of rows). The
hardware equivalent is a small per-entry age counter aged on a
periodic tick and reset on hit — the same cost class as `cnt`.

**Validation — a measured no-op.** `rvv/sweeps/viper_sweep_827.py`
legs `sc_si_lw` / `sc_si_lw_dct48` (bfs, bc, window 4096): all four
cells are bit-identical to their `sc_si` counterparts (cycles and
every prefetcher stat equal; `scalarWalksDead` = 0, `taintsDead` =
0-11 over the whole ROI). Every link in the dct48 table is live: the
12 heads / 63 links are the working set, refreshed 13M times, and no
walk ever crosses a dead one. So the residual dct48 gap is not
staleness of any kind. What the non-clearing table does differently
is *classify more instructions as chain-dependent* — 13.0M vs 11.9M
link refreshes, 9.7M vs 6.9M constant captures, 1.13M vs 1.09M
both-dependent breaks — because PT taint is never wiped, so a value
that a clear would have turned into a "constant" operand stays a
chain input and the promotion walk compiles a deeper, rarer-firing
chain (bc's backward phase: when `queue[qi]` loses IPT confidence at
a depth-level boundary it is demoted to a dependent link fed by the
tainted `qi`, and the walk continues up to the `depth_index[d]` head,
which triggers once per level instead of once per row — consistent
with equal IPT stride hits but 40% fewer landing on a compiled head,
and 4x the base misses). That is a chain *selection* question
(nearest useful head), not a liveness one. The mechanism stays,
default off, at the cost of one counter and one stamp per entry; if it
is still unused after the next campaign it should go.

**Regression set (full stack `scalar_chain_enable` +
`scalar_shape_invalidate` + `--tyche-live-window 4096`, dct24, vs the
`viper_ed0` postfix cells):**

| kernel | ed0 vs base | full stack vs base | vs ed0 | note |
|---|---|---|---|---|
| pr_vector | 1.726x | 1.940x | **+12.4%** | 1.06M adoptions, 5.2M head triggers, baseHit 7.06M / miss 59; pfIssued 26M -> 165M (140M pfHitInCache) |
| cc_sv_vector | 1.461x | 1.597x | **+9.3%** | baseHit 723K / miss 19; pfIssued 2.7M -> 20.7M |
| cc_vector | 1.303x | 1.303x | 0 | no join ever forms (chain never compiles) — bit-identical |
| sssp_vector | 1.000x | 1.000x | 0 | never discovers (strided producer) — bit-identical |
| rivec_spmv | 2.024x | 1.884x | **-6.9%** | see below |
| bfs_vector | 1.125x | 1.215x | +8.0% | (sc center 1.214x) |
| bc_vector | 0.962x | 1.067x | +10.9% | (sc center 1.066x) |

The spmv loss is the pre-existing scalar chain, not the new
invalidation: the attribution control `viper_sc_rivec_spmv` (sc alone)
is 1.824x, the full stack 1.884x — si/lw help slightly. The chain is
right (baseHit 163K / miss 505) but redundant: CSR rows are
contiguous, so viper's stream walk already covers them (ed0 coverage
0.96); the chain triples pfIssued (2.7M -> 8.6M, 5.5M pfHitInCache),
pfLate rises 10x and coverage drops to 0.86. On pr/cc_sv the same
redundancy (pfHitInCache 140M / 18M) is outweighed by the rows the
stream cannot reach. A policy that suppresses the scalar chain where
the producer's stream walk is already contiguous is the obvious next
lever; not attempted here.

## VIPER: strided (`vlse`) producer loads (2026-08-29)

**What this fork had.** Only a unit-stride `vle` could be an
index-stream producer. `StaticInst::VecMemInfo::Kind` had no strided
kind (`src/cpu/static_inst.hh`); the strided element micro-op
(`VlElementMicroInst`, `src/arch/riscv/insts/vector.hh`) reported
`None` and only tagged its *request* with the stride (`RVVExtension`,
read by nothing on the prefetch side); `VectorChainTable::dispatch()`
treated "any other memory op (strided/whole-register)" as a taint
kill (`src/cpu/vector_chain_table.cc`), so a gather fed by a `vlse`
result never linked (sssp: `vv = vlse32(base, 8)` -> `vsll` ->
`vluxei32 dist` — the "sssp never discovers" result of every campaign);
and the scalar-to-vector join in `src/cpu/o3/inst_queue.cc` was gated
on `UnitStrideLoad`. On the prefetcher side the ISA issues a `vlse`
as **one EEW-byte request per element** (`VlElementMicroInitiateAcc`,
`EA = Rs1 + Rs2*ei + offset`), against viper's chunk model of one
line-sized access per chunk (`addr != lastAddr`, stream window sized
from the access size, read-hit capture requiring a whole line inside
the access).

**What was added.**

- **Classification.** `VecMemInfo::StridedLoad`; `VlElementMicroInst::
  vecMemInfo()` reports it with `elemBytes`, the macro-op's `vl`, and
  `microIdx` = the element index (`regIdx*vlenb/EEW + microIdx`), so
  "micro 0" keeps meaning "the macro-op's first element" for the
  scalar join, which now accepts both kinds (`inst_queue.cc`, both
  insert paths).
- **Stride capture.** The stride is rs2, a scalar register, known only
  at issue. The IQ's existing chain snoop (`inst_queue.cc`, "sources
  are ready at issue") takes source slot 1 for a `StridedLoad` instead
  of the first integer source (rs1, the base); commit routes it to the
  new `VectorChainTable::captureStride()` (`commit.cc`, beside
  `armBase`/`captureScalar`). The head row carries `strided`,
  `stride`, `strideValid` and the last dispatched `vl`;
  `producerInfo()` exports `stride` (0 = unit-stride) and `vl`, and
  reports a strided head as not-found until its stride has arrived.
  `dispatch()` inserts the head on the first element micro and
  refreshes `vl` on every one — the gather-side linking is untouched
  (PT provenance is the same).
- **Chunking (`Viper::notify`).** For `info.stride != 0` a chunk is the
  span `[first, first + vl*stride)`; an access inside the current span
  is the same chunk, one outside (or backwards) opens a new one and
  latches `stride`, `phase = first % stride`, `chunkEnd`, and
  `strideStep = vlenBytes/EEW * stride` on the level entry. The stream
  walk gets `chunk_bytes = vl*stride` and the VLEN-pinned frontier
  advances by `strideStep` per `prefetch_distance` unit (`streamAhead`).
  Strides below EEW, not a multiple of it, or wider than a line are
  refused (`stridedUnsupported`) — the walk is line-granular, so a
  stride beyond the line would emit every line of the span.
- **Slot masks.** `strideSlotMask(line, EEW, stride, phase)` marks the
  element slots on the stride's phase; it rides the 16-bit
  `elemMask` the IRT / captured-line records already carry for the
  scalar levels (`registerCapture` coalescing included). Set at the
  three registration points: the stream-window departure in
  `getPacket()`, the strided read-hit capture, and the scalar chain's
  terminal hop (a future-row base line under a strided producer is
  masked from `target % stride`, so the interleaved payload words —
  sssp's weights — never become gather targets). `convertChunk()`
  honours a nonzero mask the way `convertScalarLine()` does (slots off
  the mask cost no lane). EEW >= 4 B at 64 B lines, as for the scalar
  levels.
- **Read-hit capture.** The element access that hits carries EEW
  bytes, not the line, so `captureStridedReadHit()` reads the resident
  line off the cache (`accessor.readLine`, the scalar chain's
  resident-capture path) and latches it under the slot mask; it
  retires the registration at once, so the other vl-1 element hits on
  the same line read as this producer's own re-capture and are
  suppressed by the retired tail (`stridedHitsCaptured`).

**Not covered.** `limit_gate` extents (STT announcements are `vl*EEW`
bytes, not `vl*stride`); strides wider than a line; two strided
producers sharing lines that BOTH feed gathers (the IRT keeps one
producer per line — sssp's second `vlse` feeds only arithmetic, so it
is unaffected); a shorter final strip absorbing the adjacent next
row's first accesses into the previous span (its stream trigger is
lost, its captures are not). Unit-stride producers see no change:
every new path is behind `stride != 0` / a nonzero mask, and the
table's new branch only fires on a `StridedLoad` kind that previously
killed taint.

**Validation.** sssp_vector (the only campaign kernel with `vlse`:
`rvv-index-bench/src/sssp_vector.c:109-110`) in run era `strided`
(`runs/viper_sweep_827/runs_strided/`): `viper_ed0_sssp_vector`
(discovery + gather prefetch without the scalar chain) and
`viper_sc_si_lw_sssp_vector` (row-hop chain joined through the strided
base), against the postfix cells; `viper_ed0_rivec_spmv` rerun as the
bit-neutrality control (bit-identical: 12,897,759 cycles, every ROI
stat equal).

| sssp_vector (unified, L1 attach, m16/20, VLEN 2048) | cycles | vs base | DRAM reads | pfIssued | pfUseful | coverage |
|---|---|---|---|---|---|---|
| base | 183.3M | 1.000 | 1.94M | - | - | - |
| stride deg16 | 180.9M | 1.014 | 2.02M | 181K | 74K | 0.03 |
| viper ed0, before | 183.4M | 1.000 | 1.94M | 0 | 0 | 0 |
| viper ed0, strided | 187.5M | 0.978 | 3.54M | 3.89M | 29K | 0.01 |
| full stack (sc+si+lw), before | 183.4M | 1.000 | 1.94M | 0 | 0 | 0 |
| full stack (sc+si+lw), strided | **156.6M** | **1.171** | 4.33M | 17.9M | 361K | 0.13 |

Discovery works as built: 2 heads, 1 gather linked, 258K forms
adopted, 893K strided chunks, 276K fill captures, 9K/71K read-hit
captures, `stridedUnsupported` 0. The split between the two viper
cells is the bfs/bc lesson again. Without the scalar chain the only
trigger is the stream walk, and delta-stepping relaxes vertices in
bin order, so the rows after the current one in `wnbr` are not the
rows relaxed next: the walk captures the wrong rows' neighbours and
2.24M of 3.89M gather prefetches go unused (-2.2%). With the chain
joined through the strided base (344K joins, 22.5K adoptions, 364K
head triggers, 635K base hits / 42K misses = 94%), the right rows are
predicted and the kernel is 17% faster — the first sssp win of any
campaign. The cost is DRAM reads at 2.2x base: the walk's wrong-row
traffic is still underneath, and a way to run a strided producer
chain-triggered only (no stream walk) is the obvious next knob.

## VIPER: demand-span-only emission (`demand_span_only`, 2026-08-31)

**What this fork had.** Viper's vector-side emission is a run-ahead
stream walk (`Viper::streamAhead`, `src/mem/cache/prefetch/viper.cc`):
every new chunk at a producer PC extends a window
`streaming_distance` chunks (or `prefetch_distance` whole-VLEN
chunks) PAST the current chunk, on the assumption that the next
chunks sit contiguously after it in address order. That assumption is
only true when traversal order == memory order; on bin-order
traversals (delta-stepping sssp) the whole window is cross-row
garbage — the 2026-08-29 strided-producer section measured 2.2M of
3.9M prefetches unused and DRAM reads at 2.2x base even in the
winning full-stack cell. The walk is also the capture engine's fuel:
a departing stream candidate inside a producer's window is what
registers index lines for fill capture (`getPacket()`), so it cannot
simply be skipped without replacing the registration source.

**What was added** (all in `viper.cc/.hh` +
`src/mem/cache/prefetch/Prefetcher.py`, default off, bit-neutral).
One param, `demand_span_only`, that removes the notion of a
streaming/prefetch distance entirely, for ALL producers:

- **Emission (`Viper::spanComplete`).** On a new chunk the vector
  side emits only the REMAINDER of that chunk's own span —
  `[line(addr)+blkSize, addr + vl*stride)` for a strided producer,
  `[line(addr)+blkSize, addr + vl*EEW)` for unit-stride. Line 0 is
  the demand access itself; the later lines beat the per-element
  (strided) / per-line (unit-stride) demands that would otherwise
  fetch them serially. `streamAhead` is bypassed, and with it
  `streaming_distance`, `prefetch_distance`,
  `stream_start_at_distance` and the `limit_gate` walk clamp (the
  span is demand-derived and bounded by the granted vl — there is
  nothing to gate). The high-water mark (`limitAddr`) keeps its
  meaning, so `getPacket()`'s stream-window registration captures the
  span lines exactly as it does walk candidates (slot masks
  included); `spanLinesIssued` counts them and `streamCandidates`
  reads 0 under the flag.
- **Unit-stride span chunking.** The legacy unit-stride chunk rule
  (`addr != lastAddr` — every line-split of a strip is its own chunk)
  would re-emit an overshooting span from every split, re-introducing
  lookahead. Under the flag, unit-stride chunk detection switches to
  the strided rule: one chunk = one strip `[first, first + vl*EEW)`
  via `chunkEnd` absorption, with `producerInfo().vl` (captured at
  dispatch for every producer kind, `src/cpu/vector_chain_table.cc`)
  sizing the span and `vlenBytes/EEW` as the fallback before the
  first vl arrives.
- **Future-chunk coverage moves to the scalar chain.** With no walk,
  the only future-base source is `scalar_chain_enable`'s Form B
  future-row bases (plus their chain registrations); captures arrive
  from span-line window registrations, Form B registrations and
  read-hit capture — keep `capture_on_read_hit` on, it is
  load-bearing here.

### The dynamic per-producer gate (`demand_span_chained`)

`demand_span_only` is the global, pure-form ablation: it gates every
producer, including those the scalar chain can never predict. The
dynamic gate is the shipping form of the same idea — **each producer
decides for itself, from a signal the design already computes.**

The partition is the point. A producer's base register advances one of
two ways:

1. **Arithmetic** (`ptr += vl*eew`, from the induction variable). No
   memory read is involved, so the Tyche tracker cannot root a chain
   there — `scalarChainFor`'s promotion walk needs a head that is an
   IP-stride-confident *load* (`cpu/tyche_table.cc`), and bfs's `e +=
   vl` strip advance is the recorded failure case. But such a base is
   by construction a LINEAR function of the trip count, which is
   exactly what the run-ahead walk extrapolates. The walk is the right
   mechanism.
2. **Load-fed** (`base = ja + ia[i]*8`; sssp's `off[u]`). The next
   base is data-dependent: only dereferencing ahead predicts it, which
   is what the chain's Form B terminal hop does. Here the walk is the
   wrong-row traffic the chain replaces.

So "a scalar chain terminates at this producer" is precisely the
discriminator between the two, and it is already computed —
`adoptScalarChain()` writes the terminal level with
`scalarSuccIsProducer` and `scalarSuccessorPc = producer_pc`.

**Mechanism.** `--pf-param demand_span_chained=true` (needs
`scalar_chain_enable`; the constructor rejects the combination
without it, since no producer could ever arm). At the end of a
successful `adoptScalarChain(producer_pc)` the producer's own LST row
is armed: `spanGated = true`, `spanGateGen = scalarChainKey()`
(`spanGateArmed` counts first arms and re-arms at a new key). At each
chunk event a single `span_mode` decision — computed once, before
chunk detection, so it selects the chunking rule and the emission
path together — reads

```
span_mode = demand_span_only ||
            (demand_span_chained && ps.spanGated &&
             ps.spanGateGen == scalarChainKey())
```

and routes to `spanComplete()` or `streamAhead()` accordingly
(`spanGatedChunks` counts the gated events).

**Fail-safe in both directions.** A producer with no chain never arms
and keeps its walk, so streaming kernels (axpy, the synthetic gather)
are untouched. A producer whose chain later stales — a DCT capacity
clear, or a shape/route bump under `scalar_shape_invalidate` — fails
the `spanGateGen` comparison and reverts to streaming rather than
being left with no lookahead at all. Cold iterations stream by
construction: adoption is lazy (it needs the producer linked, IPT
confidence, and a successful walk), which is also exactly when there
is no better predictor available. The arm lives in the LST row, so
`resetLearnedState()`'s existing `levelStreamTable.clear()` wipes it
per ROI with no extra code.

**Hardware cost.** One valid bit plus a small wrapping generation
stamp per LST row, written at adoption and compared at the chunk
event. No new table, no new discovery.

**Watch spot.** spmv's `ja` producer *is* load-fed (the `ia[i]`
scalar loads), so with the chain on it flips to span-only. The
recorded −7% from `sc` on spmv (2026-08-29) was pre-existing
walk/chain redundancy; this gate removes exactly that redundancy, so
it could recover that loss or forfeit the deep `pd4` lookahead the
walk was providing. Measure it rather than predicting it.

### Validation (2026-08-31)

Synthetic gather (`rvv/vector_indirect_eval.bin`, `line_locality
65536 4 67108864 16`, unified/L1/m16, VLEN 2048) and RiVec spmv
(poisson3Db), under `runs/demand_span_validation/`.

**Bit-neutrality — exact, twice.** Three runs of the same flag-off
config across the two builds (`r0` pre-span binary, `r1` span binary,
`r4` gate binary) differ only by the new counters' presence, all
reading 0. Every other stat is identical, `numCycles` included.

**Guard.** `demand_span_chained=true` without `scalar_chain_enable`
exits at `viper.cc:94` with the explanatory fatal, rather than
running as a silent no-op.

**The gate's fail-safe direction is exact.** The synthetic's producer
base advances arithmetically, so no chain is ever adopted
(`scalarChainsAdopted` 0), nothing arms, and the gated run is
BIT-IDENTICAL to the walk baseline:

| synthetic, unified L1 | cycles | pfIssued | pfUseful | L1 misses | streamCand | spanLines | armed |
|---|---|---|---|---|---|---|---|
| walk baseline (`r1`) | 268,477 | 41,551 | 22,026 | 2,545 | 8,188 | 0 | - |
| `demand_span_chained` (`r6`) | **268,477** | **41,551** | **22,026** | **2,545** | **8,188** | **0** | **0** |
| `demand_span_only` (`r2`) | 534,468 | 38,875 | 0 | 24,571 | 0 | 6,141 | - |

The global flag costs this kernel 2.0x cycles and all of its
coverage; the dynamic gate leaves it untouched. That is the entire
argument for the gate in one table.

**The arm fires, and spmv regresses.** spmv's `ja` producer IS
load-fed (the `ia[i]` scalar loads), so it arms — and the documented
watch-spot risk is what actually happened (both runs `RESULT: PASS`,
golden checksum `-2.5723580227460658`):

| spmv poisson3Db, unified L1, m16 | cycles | vs control | L1 misses | pfIssued | pfUseful | streamCand | spanLines | armed | gated chunks |
|---|---|---|---|---|---|---|---|---|---|
| `sc` control (`r8`) | 13.86M | 1.000 | 297K | 7.47M | 1.97M | 596,595 | 0 | 0 | 0 |
| `sc` + gate (`r7`) | 18.33M | **0.756** | 858K | 7.13M | 1.42M | 384,926 | 150,210 | 519 | 66,708 |

The mechanism is doing exactly what it was built to do — 519 arms,
66,708 chunk events routed to span completion, stream candidates down
35% — and the kernel is 32% slower for it.

**sssp: the opposite end, and a win.** sssp_vector on amazon0302
(`src=3 delta=16`, full stack `sc+si+lw`, strided producer), against
the recorded `strided`-era control that used the walk (`RESULT: PASS`,
checksum `109482824`):

| sssp_vector, unified L1, m16 | cycles | vs walk | DRAM reads | L1 misses | pfIssued | pfUseful | streamCand | spanLines |
|---|---|---|---|---|---|---|---|---|
| full stack, walk (control) | 156.6M | 1.000 | 4.33M | 5.95M | 17.91M | 361K | 1,255,496 | 0 |
| full stack, `demand_span_only` | **151.9M** | **1.031** | **3.35M** | 5.39M | 18.13M | **646K** | 0 | 661,063 |

Removing the walk on sssp is a straight win: 3.1% faster, **22.8%
less DRAM traffic**, L1 misses down 9.5%, and prefetch usefulness
nearly doubles (accuracy 2.0% → 3.6%) for the same issue count. The
scalar chain is untouched by the change — 634,176 base hits / 41,389
misses versus the control's 635,443 / 41,850 — which is the point:
the chain was already predicting the right rows, and the walk was
adding traffic for rows the kernel never relaxes. Against the
no-prefetcher baseline (183.3M) this is **1.207x**, up from the
strided era's 1.171x.

**What that teaches (the model refinement).** The arithmetic/load-fed
partition correctly predicts *which mechanism can supply the next ROW
BASE*. It does NOT follow that the walk has nothing else to do. A
producer can need both at once:

- the **chain** for the cross-row jump (where the next row starts),
- the **walk** for within-row readahead (the rest of a long row).

`viper.hh`'s own Form B note already says this — "the row TAIL is
demand/announcement work, not the chain's" — and the two kernels sit
at opposite ends of it. sssp's rows are roughly one strip, so the
walk has no in-row work left and everything it emits is cross-row
overrun: gating it is pure win. spmv's poisson3Db rows are long and
contiguous, so the walk's in-row readahead at `prefetch_distance=4`
is most of its value, and span completion (which stops at the current
chunk) forfeits it: L1 misses nearly triple.

With both endpoints now measured, the real discriminator is visible,
and it is NOT "is the base load-fed" — it is **row length against the
walk's lookahead**:

| | row length | walk emits | removing the walk |
|---|---|---|---|
| sssp | ~1 strip (vertex degree) | pure cross-row overrun | **1.031x** |
| spmv poisson3Db | long, contiguous | in-row readahead the demand needs | **0.756x** |

"Load-fed base" correlates with short rows in these two kernels but
does not cause the effect, so the arm is a proxy — a good one for
sssp, a wrong one for spmv.

So the gate as built is the right *classifier* attached to a
too-blunt *action*. The next iteration should suppress only the
walk's cross-row overrun — **clamp the walk window at the current
row's end** (the announced extent, or the chain-predicted next base,
which the terminal hop already computes) instead of replacing the
window with the chunk span. That single mechanism collapses to the
right behaviour at both ends automatically: where rows are ~1 strip
the clamp lands at the span and reproduces sssp's win, and where rows
are long the clamp is inert and spmv keeps its readahead. No arm bit
is needed to choose — the row end does the choosing.
`demand_span_only` and `demand_span_chained` stay as the measured
endpoints of that axis, and as proof the arm/stale/fail-safe
machinery works.

**Caveats.** `info.vl` is the last committed vl, so a row's first
strip is sized by the previous row's vl (usually VLMAX; a shorter
final strip can absorb the next row's first access — the strided
case's existing caveat, now shared by unit-stride under the flag).
Kernels whose producer base advances by arithmetic rather than a
scalar load (axpy, pathfinder, spmv's linear `ja[]` pass within a
row-ordered traversal) have no scalar chain to predict future
chunks: expect streaming coverage to collapse there by design — this
flag is the pure-form experiment for chain-predicted traversals. With
`scalar_chain_enable` on unit-stride kernels, contiguous strip
advances exceed the scalar accuracy CAM's 2-line contiguity band, so
`scalarBaseMisses` over-counts (diagnostic only, behaviour
unaffected).

## VIPER-FINAL: the shipping subset (`--prefetcher viper_final`, 2026-08-31)

**What this fork had.** `ViperPrefetcher`
(`src/mem/cache/prefetch/viper.{hh,cc}`) accumulated every mechanism
tried during the campaign as an opt-in, default-off parameter — 57 of
them by the end (`viper_final` has 41). That is right for exploration (each ablation is
bit-neutral when off, so a sweep can isolate it) and wrong as a
description of the design: reading `viper.cc` gives no signal about
which mechanisms carried their weight, and the storage accounting has
to caveat every structure with "only when this knob is set."

**What was added.** `ViperFinalPrefetcher` — the same engine with five
subsystems **removed outright rather than defaulted off**, so the code
carries no trace of them. New files
`src/mem/cache/prefetch/viper_final.{hh,cc}` (782 / 1,835 lines against
viper's 1,202 / 2,656), CLI `--prefetcher viper_final`.

Removed:

1. **Scalar level chain** (`scalar_chain_enable`) and everything it
   pulled in: the `TycheChainTable` sideband and its include, the
   IP-stride head trigger, compiled scalar levels in the LST
   (`scalarLevel`/`scalarOps`/`scalarSuccessorPc`/…), `adoptScalarChain()`,
   `convertScalarLine()`, `scalarPendingReg()`, the chain-pending
   registration map, the resident-capture path for chain lines, the
   `predictedBase` accuracy CAM, the lineage `ttl` carried through
   `IrtEntry` / `CapturedLine` / `registerCapture()`, and the
   `demand_span_chained` span gate the chain armed (with
   `LstEntry::spanGated`/`spanGateGen`). This drops viper's only
   dependence on `cpu/tyche_table.hh`, so `viper_final` never joins
   `CHAIN_TABLE_PREFETCHERS`.
2. **Residency-interval registers** (`residency_intervals`):
   the `ResidencyInterval` array, `residencyObserveDrop/Miss()`,
   `residencyCovered()`, the emission-side suppression, and the
   `lastIssuedTarget` one-shot tag feeding them.
3. **Set-pressure gate** (`set_pressure_threshold`): the per-set 3-bit
   counters, the `pfOriginLines` membership set, `pressureSet()`, the
   fill (+1) / evict (−1) / first-touch-decrement bookkeeping, and the
   emission-side drop.
4. **Stream-page registry publication**: the `tbl->registerStreamPage()`
   call in `getPacket()` that fed `StreamDemoteLRURP`. The registry
   itself lives in `VectorChainTable` and is untouched — `viper_final`
   simply stops feeding it.
5. **DRAM row-aware issue reordering** (`row_schedule_bits`):
   `rowSchedule()`, the bounded comparator window
   (`reorder_window_size`), and the `lastRow` state.
6. **Span-completion emission** (`demand_span_only`): `spanComplete()`,
   the `spanLinesIssued` stat, and the unit-stride span-chunking branch
   it required. **The run-ahead walk is the only emission policy.**

16 parameters and 22 statistics go with them.

**Why `demand_span_only` went too.** It is not a bug fix or a
throttle — it is an alternative *emission policy*, and the 2026-08-31
measurements put it at the opposite end of one axis from the walk
rather than ahead of it: sssp 1.031x (rows about one strip, so the
walk's whole window is cross-row overrun), spmv poisson3Db 0.756x
(long contiguous rows, where the walk's in-row readahead is most of
its value), synthetic gather 0.502x (a pure stream with no lookahead
left at all). A policy that doubles one kernel's cycles is a
workload-specific alternative, not a default, and the mechanism that
would be right at both ends — clamping the walk window at the current
row's end from the `vsetvl`-announced extent — is not built yet.
`viper` keeps both policies for that comparison; `viper_final` ships
the walk.

**What is preserved verbatim.** Architectural discovery through the
shared `VectorChainTable`; the collapsed linear form
`target = base + (extend(idx) >> r << s)` and its rejection path;
range-based fill capture and the IRT; per-producer slice buffers;
whole-line conversion through the shift-and-add lane array
(`conversion_lanes`); the strided (`vlse`) producer path with its
element slot masks; cross-line (`dedup_buffer_size`), vl-window,
pre-lane and eviction-clocked (`evict_dedup_entries`) dedup; the
retired-IRT tail; capture admission backpressure; the random
tail-drop throttle; batch-confidence feedback; the announced-limit
gate (`limit_gate`, so `viper_final` still joins
`REVELA_TABLE_PREFETCHERS` when it is on); multi-way consumers; and
the stall-on-full demand-clocked drain.

**Why `demand_span_chained` had to go with the chain.** The gate is
armed only at the end of a successful `adoptScalarChain()` — the
terminal level names its producer, and that is the arm signal. With no
chain, no producer ever arms and the flag is a silent no-op (viper's
constructor rejects the combination for exactly this reason).
`demand_span_only` is independent and is kept.

**Wiring** (`rvv/prefetcher_factory.py`): `viper_final` is registered in
`PREFETCHERS`, `VA_PREFETCHERS` (trains on VAs, so the MMU is
registered automatically) and `VECTOR_CHAIN_TABLE_PREFETCHERS` (needs
the per-core `VectorChainTable` on both `link_table` and the CPU's
`vector_chain_table`). It joins `REVELA_TABLE_PREFETCHERS` only with
`limit_gate=true`, matching viper. Build wiring:
`src/mem/cache/prefetch/SConscript` (`Source('viper_final.cc')` +
`ViperFinalPrefetcher` in the SimObject list) and
`src/mem/cache/SConscript` (`DebugFlag('ViperFinal')`).

**Relationship to viper.** `viper` is retained unchanged as the
research vehicle — every ablation in this document still runs there.
`viper_final` is not expected to be bit-identical to `viper` at
matching parameters in general: it is identical only for
configurations where all five removed subsystems are off, which is the
default. Any run comparing the two should assert that first.

**Status: implemented, NOT BUILT and NOT VALIDATED** (built per the
user's instruction to hold builds). Before trusting it: build, then
run a bit-neutrality check against `viper` at the shared default
configuration — same kernel, `--prefetcher viper` vs
`--prefetcher viper_final` with all removed knobs at their viper
defaults — and diff the ROI stats. Every stat that exists in both
should match exactly.

### Validation of the post-port changes (2026-08-31, build 3)

Six edit rounds landed on `viper_final` after the sweep binary was cut:
`streaming_distance` removal, demand-miss capture, vl-window and
`validBytes` removal, the `producer_slots`/STT renames plus the review
fixes, strided removal, and the switch from generation counters to
invalidate pulses (with the phase-grid iteration anchor). Build clean
after three narrowing errors the `uint8_t` fields introduced
(`std::min(unsigned, uint8_t)` has no overload; three assignments
needed explicit casts) — all caught by grepping the build log for
`error:`, since **docker exits 0 even when scons fails**.

**Port fidelity (the pre-change binary).** `viper_final` at the sweep
center is **bit-identical to the recorded `viper_ed0` cells** on both
rivec_spmv (12,897,759 cycles) and sssp_vector (187,451,234) — every
ROI stat equal, including `pfIssued`, `pfUseful`, `chunksObserved`,
`streamCandidates` and `formsAdopted`. That is the evidence that all
five removed subsystems really were default-off-equivalent in viper.

**Behaviour preservation (the new binary).** rivec_spmv is
**bit-identical again** across all six rounds — same cycles, same
2,745,922 issued / 2,123,274 useful, same 619,453 stream candidates —
with `RESULT: PASS` and the golden checksum. The new counters confirm
the invalidate mechanism is live rather than dormant:
`globalInvalidates = 1` (one DCT clear in the ROI), `formsInvalidated
= 0`, `noFormSkips = 0`. This is the load-bearing result: it says the
counter-to-pulse switch, the removals and the renames are all exactly
behaviour-preserving where they were meant to be.

**Demand-miss capture and the phase grid** (cc_vector, amazon0302,
`demand_miss_capture=true`, both `RESULT: PASS`):

| | captures | tooLate | skipped | cycles |
|---|---|---|---|---|
| `demand_capture_lookahead=0` (off) | — | — | — | 12,844,971 |
| `demand_capture_lookahead=16` | 2,715 | 911 | 0 | 12,852,757 |
| `demand_capture_lookahead=8` | 3,561 | 65 | 6,152 | 12,852,757 |

Three things fall out, and each checks a different part of the
mechanism:

- **The grid lands where it should.** At VLEN 2048 / e32 an iteration
  is 4 lines, so a lookahead of 16 should drop exactly the first line
  of every iteration: 25.0%. Measured 911/3,626 = **25.1%**.
- **`captures + tooLate` is invariant at 3,626** across both
  lookaheads — the demand-miss *trigger* is independent of where
  conversion starts, which is what the two-stage design intends.
- **The straddling-line path only appears at a non-multiple
  lookahead.** `skipped = 0` at 16 is correct, not a bug: 16 is a
  whole multiple of the line's 16 elements, so every line is entirely
  before or entirely after the point. At 8 the partial path engages
  (6,152 elements skipped). The residual `tooLate = 65` at lookahead 8
  is also real: `start = 8` overruns a line only at EEW = 8, and
  cc_vector has a `vluxei64` producer alongside its e32 ones.

**What it does not show.** Demand-miss capture costs ~8K cycles
(+0.06%) and buys nothing measurable here, exactly as predicted: the
path only fires on lines that actually miss, and 3,626 of those
against 490K issued prefetches is a rounding error. A lookahead sweep
measures recovery traffic, not steady state; to make the path dominate
it needs pairing with a short frontier.

---

## The walk confidence throttle (`walk_throttle`, 2026-08-31) — REMOVED 2026-09-01

**Status: deleted from `viper_final`.** Kept here for the measurements.
It demoted bc from 0.962x to 0.983x by cutting 57% of its prefetches
(`walk_confidence_down=4`), but every operating point still lost to no
prefetcher, and it was structurally blind on sssp (see the granularity
note at the end). `demand_miss_capture` and
`demand_capture_lookahead` were removed in the same pass, having
measured at +0.06% cycles and no benefit. VIPER-FINAL keeps only the
unconditional walk.

## The walk confidence throttle (`walk_throttle`, 2026-08-31)

### What gem5 already had

Nothing. gem5's own feedback-directed prefetching lives in
`src/mem/cache/prefetch/queued.cc` only as a *queue-pressure* bound
(`queue_size`, `queue_squash`, `queue_filter`) — it limits how many
prefetches may be outstanding, never whether a given *stream* deserves
to be prefetched at all. The classic accuracy/lateness/pollution
feedback of Srinath et al. (HPCA'07) has no implementation in this
tree. Every stride-style prefetcher here (`StridePrefetcher`,
`IndirectMemoryPrefetcher`) instead gates on a *pattern* confidence:
`confidence_counter_bits` in
`src/mem/cache/prefetch/indirect_memory.cc` counts how often the
observed delta matched the predicted one. That is a question about
whether the address *formula* is right.

VIPER-FINAL's walk never had even that, deliberately: the trigger is
architectural. `vle32.v` *is* a unit-stride load, so
`streamAhead()` (`src/mem/cache/prefetch/viper_final.cc:597`) ran at
the full `prefetch_distance` for every producer, forever, and the
comment at its head said so — "no confidence: the opcode said
unit-stride".

### Why the opcode is not enough

The opcode is a promise about *one* gather's addresses, not about the
sequence of gathers. Two workload shapes were measured to disagree:

| | index array traversed | walk outcome |
|---|---|---|
| CSR spmv (`poisson3Db`) | in **address order** — row *r+1*'s `ja[]` begins where row *r*'s ends | frontier is always on the next thing read; 1.47–1.63× |
| `bc_vector`, `sssp_vector` | in **visit order** — `u = queue[i]`, then `nbr[off[u]]` | frontier reads rows the kernel will not go to next; 0.96×, ~0.7% accuracy |

Both are unit-stride *within* a gather, so nothing in the ISA
separates them. What separates them is whether the *next* gather
continues the last one's address range, and that is only observable at
run time. bc and sssp are not mispredicted-formula cases the indirect
confidence counter would catch — the formula is right, the *rows* are
wrong.

### The mechanism

One saturating counter per STT entry
(`SttEntry::confidence`, `src/mem/cache/prefetch/viper_final.hh`),
scaling that entry's own frontier:

```
distance(c) = 0                                    if c == 0
              min(prefetch_distance, (c >> S) + 1) otherwise
```

With the defaults (`walk_confidence_max=15`, `walk_confidence_shift=2`,
`prefetch_distance=4`) the ladder is `c>=12 → 4`, `8..11 → 3`,
`4..7 → 2`, `1..3 → 1`, `0 → walk off`. It is a shift and a clamp, so
in hardware it is a wire, not an arithmetic unit.

**New entries start saturated.** The opcode is real evidence and
unit-stride is the common case; the throttle exists to demote the
streams that disprove themselves, not to make every stream earn its
first prefetch.

**The scoring signal is free.** `updateWalkConfidence()`
(`viper_final.cc:562`) runs on each new chunk *before* `lastAddr` and
`limitAddr` are updated, so `limitAddr` is still the frontier the
previous walk left. The counter asks one question:

```
consumed = (addr >= lastAddr) && (blockAddress(addr) < limitAddr)
```

— did the demand stream walk *into* the lines the walk fetched? If so
those prefetches were demanded: step up. If not, the stream jumped
(backwards, or clean past the frontier) and every line still
outstanding in that window was fetched for a row the kernel never went
on to read: step down. **No prefetched line is tracked individually**
— the frontier `limitAddr` already *is* the window the counter is
evaluated over, so there is no timer, no per-line use bit, and no
PA→producer attribution table. The whole cost is the counter.

### The shadow frontier

A demoted entry (`confidence == 0`) would otherwise be trapped: with
nothing emitted, `limitAddr` stops advancing, the demand stream
inevitably passes it, and every subsequent event scores as "not
consumed". Recovery would be impossible.

So a demoted entry still **runs the walk as a shadow**: the window is
computed and `limitAddr` raised, but the `addresses.push_back()` is
skipped (`streamAhead(..., shadow=true)`). The shadow uses a
one-chunk frontier — the minimum the test needs. A demoted stream that
becomes address-ordered again therefore starts scoring `consumed` and
climbs back out; one that stays jumpy stays at zero, at the cost of an
adder and a compare per chunk and no memory traffic at all.

### Scope: the walk, not the engine

`streamAhead()` is the engine's fuel — every capture comes from a line
it emitted — so silencing it would take the *indirect* half down with
it. That is the wrong coupling: a stream whose row *order* defeats the
frontier can still have perfectly good gather targets inside the row
the demand is on. `demand_miss_capture` (added in the section above) is
exactly the path that keeps conversion alive without a frontier, so by
default a fully demoted producer keeps converting off its demand
misses. `walk_throttle_stops_capture=true` couples them, to measure a
producer switched off outright.

### Parameters (`src/mem/cache/prefetch/Prefetcher.py`)

| param | default | meaning |
|---|---|---|
| `walk_throttle` | `False` | arm it; bit-neutral when off (`walkDistance()` returns `prefetch_distance` unconditionally and no counter is touched) |
| `walk_confidence_max` | 15 | saturating max and a new entry's starting value; 15 is a 4-bit counter |
| `walk_confidence_shift` | 2 | the confidence→distance shift |
| `walk_confidence_up` | 1 | step on a consumed window |
| `walk_confidence_down` | 1 | step on an abandoned window |
| `walk_throttle_stops_capture` | `False` | at zero, also stop demand-miss capture |

### Stats

`walkConfidenceUp` / `walkConfidenceDown` (the scoring events),
`walkThrottledOff` (chunk events at a demoted entry),
`walkDistanceSum` (÷ `chunksObserved` = the mean realised lookahead —
the single number that says how hard the throttle actually bit), and
`walkThrottledCaptures`.

### What it cannot catch

The counter scores *whether the demand stream arrived*, not *whether
the prefetch was useful when it did*. A stream that walks contiguously
forever but is always too late, or whose lines are always already
resident, scores as fully consumed and keeps the maximum distance.
Catching that needs a per-line use bit and PA→producer attribution —
the storage this design deliberately avoids. The failure mode it is
built for (bc, sssp) is a row-order failure, which this does see.

---

## Rejecting strided (vlse) producers (2026-09-01)

`viper_final` was documented from birth as tracking "only unit-stride
index loads" — item 10 of the removal list in
`src/mem/cache/prefetch/viper_final.hh`. The code did not do that. The
span model for strided producers was deleted, but nothing rejected the
producers themselves, so they were **mis-modelled as unit-stride**,
which is worse than either tracking or ignoring them.

`VectorChainTable::producerInfo()`
(`src/cpu/vector_chain_table.cc:819`) returns a strided head as a
usable producer and reports its stride:

```cpp
if (dct[idx].strided && !dct[idx].strideValid) {
    return out;              // only rejects an UNCAPTURED stride
}
out.found  = true;
out.stride = dct[idx].strided ? dct[idx].stride : 0;   // reported
```

`viper_final.cc` never read `info.stride`, and `streamAhead()` runs
before and independently of the `info.linked` gate, so an unlinked
strided producer was walked too. A `vlse32` issues one access per
element, 4 bytes each, so every element tripped `new_chunk` and got
its own walk: a 4-byte window placed `prefetch_distance * VLEN / 8` =
1024 bytes ahead of a cursor advancing by the stride.

**Measured on sssp_vector** (the only one of the seven campaign
kernels containing `vlse` — 2 sites; pr, bfs, cc_sv, cc, bc and spmv
have none):

| | span model present | span model deleted |
|---|---:|---:|
| chunksObserved | 893,489 | **3,851,851** (4.3x) |
| pfIssued | 3,893,196 | **4,545,324** |
| cycles | 187,451,234 | 187,184,764 |

Removing the feature made the prefetcher issue 17% *more* prefetches.
It also made the walk confidence throttle inert on sssp: consecutive
strided elements advance the cursor far less than the 1024-byte
frontier, so `blockAddress(addr) < limitAddr` held on 84% of events
and only 104 of 3.85M chunk events ever demoted.

**The fix** is one guard in `calculatePrefetch`, making the code match
the contract the header already stated:

```cpp
if (!info.found || info.stride != 0) {
    drainEmission(addresses);
    return;
}
```

The chain table is untouched — it is shared with GDP, vhybrid and
viper, which have their own strided handling. Rejection is local to
`viper_final`.

---

## VIPER-FINAL: redundant-state trim (2026-09-02)

**Why.** A hardware pass over `viper_final` and its design doc
("Viper Final in Silicon") looking for state that another field
already determines, and for mechanisms that existed twice. Everything
below is deleted from `src/mem/cache/prefetch/viper_final.{hh,cc}`,
not defaulted off. The engine's behaviour is meant to be unchanged
(see *Expected effect*); what changes is how many bits and how many
paths say the same thing.

**What gem5 had, and what each item duplicated.**

1. **The op-list adoption path** — `ConsumerGroup::form`, the
   engine-side `LinearForm` and `collapse()`, and the
   `tbl->pipelineConfig()` walk in `calculatePrefetch`. The chain
   table's backward walk already folds every chain (`foldChain`,
   `src/cpu/vector_chain_table.cc`) and pushes the result into the
   IPT here (`installForm`), so the engine held a second fold of the
   same algebra and a second copy of the form in every consumer way.
   `folded_forms` is now REQUIRED (`fatal_if` in the constructor) and
   the config script sets it for `--prefetcher viper_final`
   (`rvv/riscv-rvv-se-ara-prefetcher.py`). The lane array reads the
   IPT record directly (`wayForm()`); `applyForm`/`preLaneKey` take a
   `VectorChainTable::FoldedForm`.

   *Latent bug this exposed:* with `folded_forms` on, the old code
   never adopted anything. `producerInfo().linked` is
   `!consumers.empty()` (`vector_chain_table.cc`, producerInfo), and
   `consumers[]` is written only inside the `if (!foldedForms)` branch
   of the walk, so the "bit-neutral" folded path silently streamed
   without converting. The engine no longer reads `linked` at all —
   the IPT is its own record of the same fact — so the path is now
   the only path, and it works.

2. **`ConsumerGroup::configured` and `configuredCount` /
   `producer_slots` gating, plus the `ipt_sets` param.** A way is live
   iff its IPT way holds a valid form; a producer holds a slot iff it
   holds an IPT set. `producer_slots` now sizes the IPT
   (`ipt.resize(producerSlots)`); `ipt_sets` is removed from
   `ViperFinalPrefetcher` (`Prefetcher.py`). `producerSlotsSaturated`
   folds into `iptSetConflicts`. Per-way liveness is one function,
   `wayForm(pc, way)`, and producer liveness is `producerLive(pc)`.

3. **`IptWay::consumerDctPtr`'s empty encoding (−1) beside
   `FoldedForm::valid`.** Two occupancy bits per way. A push always
   arrives valid (the walk sets `f.valid = true` right before
   `installForm`, `vector_chain_table.cc`), so `form.valid` IS the
   way's liveness; the pointer is only the routing key an invalidate
   names. `invalidateForm` now clears the form rather than leaving an
   "occupied but invalid" way; a cleared way keeps its stale pointer
   (don't-care, like a free set's tag) so a re-walk of the same gather
   lands in the same way and the dedup record beside it stays put.
   Hardware width: 3 b pointer, not 4.

4. **`SttEntry::valid`.** Always true. The entry is created by
   `streamTrackingTable[pc]` in `calculatePrefetch` and `valid` was set
   on the same path before anything else could read it; the only clear
   was the map clear. Presence in the map is the bit (in hardware, the
   row's tag valid). `calculatePrefetch` now uses `try_emplace` to
   detect the first chunk event, which also sizes `groups` once to
   `consumers_per_producer` (the ways are physical) and latches
   `elemBytes` from the producer query on every chunk.

5. **`SttEntry::latchValid`.** Derivable: the latch is live iff
   `next < targets.size()` (`ConvertedLine::pending()`); flushes call
   `ConvertedLine::clear()`. In hardware the latch already needs a
   count register, so valid = cursor < count.

6. **The polled `globalInvalidate` flag / `takeGlobalInvalidate()`**
   in `VectorChainTable`, checked by `applyGlobalInvalidate()` on
   every `calculatePrefetch`. `clearAll()` already pushed
   `formSink->invalidateAllForms()`; only the unresolved-head case in
   `noteOperandChange()` raised the flag without pushing. That case
   now pushes too, and the flag and method are removed from the shared
   table (viper_final was their only consumer; gdp/viper/vhybrid use
   `operandGen()`). `invalidateAllForms()` clears the IPT and flushes
   every producer's ILQ and latch itself.

7. **The whole-producer "no form" flush blocks in `drainEmission`**
   (three sites) and the duplicated `anyConfigured` check in
   `notifyFill`. Replaced by an invariant: a producer's ILQ and latch
   are non-empty only while it is live, because captures are admitted
   only for live producers (`getPacket`, `notifyFill`,
   `captureFromReadHit`'s caller) and the invalidate that frees a
   producer's last way flushes both (`flushConversion`). The
   per-target way check (a way invalidated mid-latch forfeits its own
   targets) stays. Helpers `clearLineState` / `closeLineDedup` replace
   the three hand-copied dedup-window closes.

8. **The duplicated post-lane dedup path in `convertChunk`.** The
   `pre_lane_dedup` if/else were two near-identical loops whose
   seen-sets held different things (target lines vs. folded keys).
   They are provably the same set: with `X = extend(idx) << shift`,
   `line = (base + X) >> 6 = (base >> 6) + ((X + (base & 63)) >> 6)`
   exactly, so `preLaneKey` names the target line and the emitted set
   is identical in both placements. One loop now runs the key compare
   first; `pre_lane_dedup` decides only whether a hit spends a
   `conversion_lanes` slot (true: the lane is clock-gated; false: it
   fired first). `elementsPreDeduped` is dropped (it equalled
   `targetsDeduplicated` when the knob was on).

**Stats removed:** `formsAdopted`, `formsMemoHits`, `chainNotReady`,
`chainsRejected`, `producerSlotsSaturated` (→ `iptSetConflicts`),
`elementsPreDeduped`. `formsInstalled` is the replacement for
`formsAdopted`. The two `viper_final` sweep parsers
(`rvv/sweeps/viper_final_sweep.py`, `viper_final_ed_sweep.py`) now read
`formsInstalled` and the table's `formsPushed` instead of
`formsAdopted`/`linksFormed` (the latter is never incremented under
folded forms).

**Params removed:** `ipt_sets`. `pre_lane_dedup`'s doc now states the
slot-accounting-only semantics. The script also wires `vlen` for
`viper_final` as it did for `viper` (the sweeps passed it explicitly,
so the documented command line had been fataling without it).

**Expected effect.** Bit-neutral in cycles and prefetch counts against
the 2026-08-31 binary at the sweep centre (`conversion_lanes=0`), with
three known, small timing deltas: (a) a producer becomes live at the
gather's commit that pushes its form rather than at the producer's
next chunk event, so a capture landing in that window now converts
instead of counting a `noFormSkips`; (b) a global invalidate flushes
buffered work at the pulse rather than at the next drain (same lines
lost, different stat); (c) at `conversion_lanes > 0` with
`pre_lane_dedup=false` the slot accounting is preserved exactly, and
with it `true` the emitted set is unchanged by construction.

**Status: NOT BUILT, NOT VALIDATED** (build held per CLAUDE.md). A
host `g++ -std=c++17 -fsyntax-only` of `viper_final.cc` and
`vector_chain_table.cc` against `build/RISCV` passes with no new
warnings. Validation to run: the spmv/poisson3Db centre from the
2026-08-31 campaign, diffing `pfIssued`, `pfUseful`, `chunksObserved`,
`streamCandidates`, `fillsCaptured`, `targetsGenerated` and cycles.
`formsInstalled` should be nonzero and `formsPushed` (chain table)
should match it minus `iptSetConflicts + iptWaysExhausted`.

**Design doc updated:** "Viper Final in Silicon"
(https://claude.ai/code/artifact/80c371ec-cba7-4881-9eeb-fb26a0924043),
§1 adoption/invalidate text, §2 slot text, §4 dedup placement, §6
widths (IPT way 73 b, latch count), §7 field entries, §8 ledger rows
7 and 9, footer revision.

## VIPER-FINAL: IPT way extension code (2026-09-02)

**Existing method.** `VectorChainTable::FoldedForm`
(`src/cpu/vector_chain_table.hh`) held the index extension width as
`unsigned extBits`, a magnitude in bits. The hardware accounting
charged it 7 b (the value 64 needs seven bits), so the IPT way was
73 b = consumer pointer 3 + form 70 (valid 1, base 48, shift 6,
rshift 6+1, ext 7+1). The DCT row already stored the same quantity
(`extWidth`) as a 3-bit *encoding* — the width is always a point on
the SEW ladder {none, 8, 16, 32, 64}, five states — so the two
records disagreed on the width of one field.

**What changed.** The form now stores the code, not the magnitude:

- `FoldedForm::extCode` (`uint8_t`, 0 = none, 1..4 = extend from
  8/16/32/64 bits) replaces `extBits`. `encodeExtWidth()` maps a
  width onto the ladder and `extWidth()` decodes it back
  (`4 << code`). Both live on the struct.
- `foldChain()` (`src/cpu/vector_chain_table.cc`) writes the code at
  both extension sites (`SExt`/`ZExt` and `WMul`/`WMulU`) and now
  **refuses the chain** if the width is off the ladder, instead of
  silently folding it. No legal RVV encoding produces such a width
  (`vsext.vfN` sources are 8/16/32, widening multiplies use SEW), so
  the rejection is a guard, not a behaviour change.
- `ViperFinal::extendRaw()` (`src/mem/cache/prefetch/viper_final.cc`)
  decodes the code once and applies the same sign/zero extension as
  before. `applyForm()` and `preLaneKey()` are unchanged.

The other prefetchers (`viper`, `vhybrid`, `vector_tyche2`) keep
their own `LinearForm` copies with `extBits`; only `FoldedForm` — the
one form the IPT way holds — changed.

**Storage.** IPT way 73 b → 69 b (form 70 b → 66 b); IPT at two
single-way sets 29 B → 28 B; engine 2,911 B → 2,910 B; control
structures 824 B → 823 B. The shifts keep their 6-bit ranges because
they feed a shifter; the extension only selects a mux, which is why
the code is enough.

**Behaviour.** Bit-neutral on any chain the old fold accepted: the
decoded width is identical, so every target address is identical.
Host `g++ -std=c++17 -fsyntax-only` of both files passes; gem5
rebuild pending.

**Design doc updated:** "Viper Final in Silicon"
(https://claude.ai/code/artifact/80c371ec-cba7-4881-9eeb-fb26a0924043),
§1 adoption text, §6 IPT row and totals, §7 IPT way and folded-form
entries, footer revision.

## VIPER-FINAL as Verilated RTL (`--prefetcher viper_rtl`, 2026-09-02)

This section uses ASD-STE100 Simplified Technical English.

### What gem5 provides

A prefetcher is a `prefetch::Base` SimObject
(`src/mem/cache/prefetch/base.hh`). The cache calls it through probes:
`notify()` for a demand access, `notifyFill()` for a fill,
`notifyEvict()` for a data update, and `getPacket()` when the cache
wants a prefetch (`src/mem/cache/base.cc`, `sendMSHRQueuePacket`).
`Queued` (`src/mem/cache/prefetch/queued.cc`) adds the prefetch queue,
the translation queue, and `insert()`. `ViperFinal`
(`src/mem/cache/prefetch/viper_final.cc`) is the VIPER-FINAL algorithm
on top of `Queued`. The CPU-side `VectorChainTable`
(`src/cpu/vector_chain_table.cc`) pushes folded forms into the
prefetcher through `VectorChainTable::FormSink`.

### What was added

The repository `~/viper_rtl` holds a SystemVerilog implementation of
`ViperFinal` and of the `Queued` queue. gem5 runs it through Verilator.
The additions to this tree are:

| File | Role |
|---|---|
| `src/mem/cache/prefetch/viper_rtl.hh`, `viper_rtl.cc` | The wrapper class `prefetch::ViperRtl`. It derives from `Base` (not `Queued`), because the queue is in the RTL. It also implements `VectorChainTable::FormSink`. |
| `ext/viper_rtl/` | The Verilated model (`gen/`), the Verilator runtime, the driver (`harness/`), and a `SConscript` that builds them into `libviper_rtl.a`. |
| `src/mem/cache/prefetch/Prefetcher.py` | `class ViperRtlPrefetcher(BasePrefetcher)`: the `ViperFinalPrefetcher` knobs plus the `QueuedPrefetcher` knobs and `trace_file`. |
| `src/mem/cache/prefetch/SConscript`, `src/mem/cache/SConscript` | The SimObject, the source, and the debug flag `ViperRtl`. |
| `rvv/prefetcher_factory.py`, `rvv/riscv-rvv-se-ara-prefetcher.py` | The CLI name `viper_rtl`, its chain-table and MMU wiring, `vlen` from `--vlen`, and `folded_forms`. |

`make gem5-install` and `gem5/install_hooks.py` in the repository write
these files. Every hook line carries the marker `viper_rtl-hook`.

### How the wrapper works

Each `Base` or `FormSink` call becomes one RTL event. The driver
(`ext/viper_rtl/harness/viper_rtl_driver.cc`) sets the event ports,
pulses `ev_valid`, and clocks the model until its `busy` output is low.
The gem5 tick does not advance during these cycles. The RTL therefore
gives the same prefetches at the same ticks as `ViperFinal` (exact
mode). The wrapper reports the RTL cycles as `rtlCycles`, `rtlEvents`,
and `rtlMaxCyclesPerEvent`.

The RTL has three service ports that the wrapper answers with gem5
objects:

- `xl_req_*`: an address translation. The wrapper builds a `Request` like `Queued::createPrefetchRequest()` and calls `mmu->translateTiming()`. A synchronous completion is answered in the same RTL cycle. A later completion becomes an `EV_XL_RESP` event.
- `snoop_*`: `inCache() || inMissQueue()` on the hosting cache's `CacheAccessor` (`cache_snoop`).
- `hit_rd_*`: a 64-byte window of the demand packet's payload for `capture_on_read_hit`.

The wrapper builds the prefetch `Packet` when the RTL pops a queue
entry, exactly like `Queued::DeferredPacket::createPkt()`. The
self-clocked drain of `ViperFinal::drainTick()` is kept: the wrapper
schedules a drain event one cycle later when the RTL reports pending
work.

The wrapper mirrors every RTL statistic pulse onto a gem5 statistic
with the `ViperFinal` and `Queued` names, so the sweep parsers need no
change.

### Assumptions

The RTL is bit-exact with a gem5-free reference copy of `ViperFinal`.
The reference differs from gem5 in documented points (repository
`README.md`, section 4): producer visit order, a bounded STT with LRU
replacement, the dedup window in the producer slot, a xorshift RNG,
fixed-point fractions, and tolerance of stale translation completions.

### How to run

1. In `~/viper_rtl`: `make verilate` (add capacities such as `ILQ_ENTRIES=8` when a run needs them).
2. `make gem5-install GEM5_ROOT=~/gem5`, then `python3 gem5/install_hooks.py ~/gem5`.
3. Build gem5 with `scons build/RISCV/gem5.opt -j$(nproc) --ignore-style` in the docker container.
4. Run with `--prefetcher viper_rtl` and the `viper_final` wiring. Compare `stats.txt` with a `--prefetcher viper_final` run.

Status at the time of writing: the wrapper compiles against the gem5
headers (host syntax check) but was not built into `gem5.opt`. The
repository owner builds gem5.

## VectorChainTable: affine-only transform decode (2026-09-02)

### What gem5 provided

Nothing: the vector chain table is this fork's own
(`src/cpu/vector_chain_table.cc`). Its forward propagation decodes each
OP-V arithmetic instruction that consumes a tagged register with
`decodeTransform()` and either extends the chain with a transform row
or breaks it (`chainsBroken`).

### What changed

`decodeTransform()` accepted `vrsub`, `vand`, `vor` and `vxor` as
transforms. They were left over from the op-list replay engines, which
applied each op to a captured value. `foldChain()`, the only consumer
of a chain since `folded_forms` became required, cannot fold them: it
folds a chain into `(elem << shift) + bias`, and a negated scale or a
bitwise op has no such form, so it rejected the chain in the backward
walk (`foldsRejected`).

That left two places deciding what a chain is, with different
answers. A gather behind a `vxor` was walked, folded, rejected, and its
`linked` bit set so the walk would not retry, which was the only case
in which `linked` carried information the IPT does not.

The decoder now accepts only the ops `foldChain()` folds: `vadd`,
`vsub.vx`, `vsll`, `vsrl`, `vsra`, `vmul.vx`, `vwmul[u|su].vx`,
`vsext`, `vzext`. `vrsub` and `vand`/`vor`/`vxor` are chain breakers.
The four `VOp` values were deleted from the enum and from GDP's replay
switch (`src/mem/cache/prefetch/gdp.cc`), which was the only other
reader.

`foldChain()` still rejects on operand *values* the decoder cannot see
at dispatch: a `vmul.vx` by a non-power-of-2 scalar, a right shift
after a scale, a sign-mismatched `vsra`, an extend after a scale. Those
scalars arrive at issue (`captureScalar`), so the rejection has to stay
in the walk. Across every `folded_forms` run on disk at the time of
writing, `foldsRejected` was 0.

Effect on results: GDP loses replay of `vrsub`/`vand`/`vor`/`vxor`
chains. No run on disk exercised them (`foldsRejected` = 0 on the
folding prefetchers, and the same chains would have reached GDP). Any
chain that did use one now shows up as `chainsBroken` instead.

## VIPER-FINAL: slice width moves from the STT to the IPT (2026-09-03)

**Existing method.** `ViperFinal::SttEntry::elemBytes`
(`src/mem/cache/prefetch/viper_final.hh`) held the producer's EEW/8 —
the width the lane array slices a captured index line at. It was
rewritten from the chain table's producer query at every chunk event
(`calculatePrefetch`, `src/mem/cache/prefetch/viper_final.cc`) and
read once, in `convertChunk`. The hardware accounting charged every
STT row 3 b for it.

**Why it was redundant there.** Three facts, each from the code:

- The value is per producer and constant. The DCT head row writes
  `elemBytes` once at allocation (`VectorChainTable::dispatch`,
  `src/cpu/vector_chain_table.cc`) and afterwards refreshes only `vl`.
  A capacity clear broadcasts `invalidateAllForms`, which frees every
  IPT set, so a re-allocated head with a different width always
  arrives on a fresh install. The per-chunk refresh therefore rewrote
  the same value every time.
- The only reader runs on a live producer. `convertChunk` runs at the
  drain on a queued captured line. Captures are admitted only for
  producers that hold an IPT set (the `producerLive` gates on the
  read-hit capture, the fill-window match, and the fill capture in
  `viper_final.cc`), and the invalidate that frees a set's last way
  flushes that producer's index line queue and latch. An IPT lookup at
  conversion therefore always finds the set.
- Every unit-stride vle owns an STT row, producer or not (the row is
  the stream window). On a stream-only row the width was never read.

**What changed.**

- `VectorChainTable::FormSink::installForm`
  (`src/cpu/vector_chain_table.hh`) takes `elem_bytes`. The push site
  in `dispatch()` passes `dct[ptr].elemBytes`, the head row it already
  reads the producer PC from.
- `ViperFinal::IptSet::elemBytes` holds it once per producer set,
  beside the ways. `installForm` writes it on every push (a re-push
  rewrites the same value); `convertChunk` reads it through
  `iptFind(pc)` and asserts the set exists. `SttEntry::elemBytes` and
  its per-chunk write are gone. The producer query in
  `calculatePrefetch` stays, for `found` and `stride`.
- `ViperRtl::installForm` (`src/mem/cache/prefetch/viper_rtl.hh/.cc`)
  takes the argument and does not forward it: the RTL in `~/viper_rtl`
  still keeps `stt_elem` on its STT, written from the access event's
  `producer_elem_bytes`. Moving it onto the RTL's IPT set is a change
  in that repository and was not made here.

**Storage.** STT row 140 b → 137 b (8 rows: 140 B → 137 B). IPT set
tag 40 b → 42 b: the four legal widths {1,2,4,8} are a 2-bit log2
code, and no "unset" state is needed because set occupancy is already
the ways' valid bits (the STT's 3 b had counted one). IPT at two
single-way sets 218 b → 222 b, still 28 B. Engine 2,910 B → 2,907 B;
control structures 823 B → 820 B; whole design still ≈ 2.97 KB.

**Behaviour.** Bit-neutral: the width read at every conversion is the
same DCT head-row value as before. Host
`g++ -std=c++17 -fsyntax-only` of `vector_chain_table.cc` and
`viper_final.cc` passes; `viper_rtl.cc` cannot be host-checked in this
tree (its params header is not generated). gem5 rebuild pending.

**Design doc updated:** "Viper Final in Silicon"
(https://claude.ai/code/artifact/80c371ec-cba7-4881-9eeb-fb26a0924043),
§1 adoption text and the producer-query aside, §2 STT contents, §4
slice source, §6 STT and IPT rows and totals, §7 IPT set (new
`elemBytes` entry, struct sources corrected to `ViperFinal::IptSet` /
`IptWay`), STT entry, and DCT `elemBytes`, footer revision.

## Removed: GDP, VIPER (research build), VTyche2, and VHybrid (2026-09-04)

Four experimental prefetchers were deleted now that `viper_final` (and
its RTL twin `viper_rtl`) is the shipping design. The sections above
that describe them are kept as the design record; the code is gone:

- **gdp** (`mem/cache/prefetch/gdp.cc/.hh`, `GDPPrefetcher`): the
  transform-chain-replay Gather Dataflow Prefetcher.
- **viper** (`mem/cache/prefetch/viper.cc/.hh`, `ViperPrefetcher`): the
  full research VIPER with every subsystem `viper_final` later cut
  (scalar level chain, eviction-clocked dedup, set-pressure gate,
  residency intervals, demand-span-only, strided producers, ...).
- **vtyche2** (`mem/cache/prefetch/vector_tyche2.cc/.hh`,
  `VectorTyche2Prefetcher`): the decoupled metadata/data-lead variant.
- **vhybrid** (`mem/cache/prefetch/vhybrid.cc/.hh`,
  `VHybridPrefetcher`): ReVeLA streams driving VIPER's indirect half.

What went with them: the `GDP`, `Viper`, `VTyche`, and `VHybrid` debug
flags (`src/cpu/SConscript`, `src/mem/cache/SConscript`), the
`--prefetcher gdp|viper|vtyche2|vhybrid` choices and the
`scalar_chain_enable` unified-cache check in
`rvv/riscv-rvv-se-ara-prefetcher.py`, their `rvv/prefetcher_factory.py`
entries (`needs_chain_table` no longer has a viper opt-in;
`REVELA_TABLE_PREFETCHERS` is now just `revela`), and the orphaned
`gdp_test`/`gdp_ext_test` targets in `rvv/Makefile.tests` (their
sources were already gone).

What stayed, because `viper_final`/`viper_rtl` still use it: the
`VectorChainTable` (`src/cpu/vector_chain_table.*`), the
`TycheChainTable` (standalone `tyche`), the `RevelaStreamTable`
(`revela`, and `viper_final`'s opt-in `limit_gate`), the
`StreamDemoteLRURP` replacement policy, and the
`--vector-dct-entries` / `--vector-max-transform-stages` /
`--stream-demote*` script flags. The hierarchy wiring in
`rvv/*_cache_hierarchy.py` was already guarded by parameter presence
(`"link_table" in prefetcher._params`), so it needed only comment
updates. Sweep scripts and run directories under `rvv/sweeps/` and
`runs/` that targeted the deleted prefetchers were left in place.

## VIPER-FINAL: the staleness abort — measured, then deleted (2026-09-05)

**What gem5 does.** `QueuedPrefetcher` (`src/mem/cache/prefetch/queued.cc`)
has no notion of a candidate being "too late": whatever `calculatePrefetch`
pushes sits in the queue until the cache pulls it or the queue overflows.
Timeliness is the prefetcher's own problem.

**What viper_final had.** Its drain loop (`ViperFinal::drainEmission`,
`src/mem/cache/prefetch/viper_final.cc`) compared every buffered or latched
index line against the producer's current demand line and discarded lines
the walk had already passed: a latched batch whose `lineVaddr` was below
`blockAddress(ps.demandAddr)` was cleared before emission, and a queued
`CapturedLine` in the same position was popped before conversion. Both
sites counted `stalenessAborts` and forfeited the line's dedup entries
(`clearLineState`). The reasoning was that targets converted from such a
line issue behind the demand and can only pollute.

**Measurement.** A `staleness_abort` Bool param was added for one day to
gate both sites, and the Sep 2 campaign centre was re-run with it off
(driver `rvv/sweeps/viper_final_nostale_sweep.py`, results
`runs/viper_final_nostale/viper_final_nostale.xlsx`; a same-binary parity
run with the abort on reproduced the Sep 2 spmv cycles bit-exactly). The
abort fired on 0.04 % (spmv) to 1.3 % (cc_vector) of registered captures
in the Sep 2 runs; removing it moved every kernel by less than 0.07 %
of cycles in either direction (spmv +0.064 %, cc_vector +0.039 %, pr
0.000 %, bfs −0.000 %, cc_sv −0.014 %, bc −0.012 %, sssp identical with
zero prefetches) and the geomean speedup from 1.3278 to 1.3276. The
lines that survived added at most 0.2 % more prefetches per kernel,
most of which the cache dropped as already resident.

**What was deleted.** The two comparisons, the `stalenessAborts` stat,
the one-day `staleness_abort` param and its `ViperFinal::stalenessAbort`
member. `CapturedLine::lineVaddr` and `ConvertedLine::lineVaddr` stay:
the latch's copy is the batch tag that `drop_batch_confidence` keys
`emittedFrom`/`abortedBatches` on, and the captured line's copy feeds
it (and the debug trace). Hardware saved: one line-address compare per
producer per drain event. `capture_queue_gate` still exists and still
sheds captures under queue backpressure; its parameter text no longer
claims those captures would be aborted — they now convert and issue
late, which the measurement above says is harmless.

**Not changed.** `ViperRtl` and the SystemVerilog model in `~/viper_rtl`
still implement the abort (`ST_stalenessAborts` in
`ext/viper_rtl/harness/viper_rtl_driver.hh`). The RTL is therefore no
longer an exact port of `viper_final` on this point until its abort is
removed too; runs that compare the two will diverge wherever the RTL
aborts a batch (≤1.3 % of captures on the campaign kernels).

## VIPER-FINAL: persistent seen-set (`seen_set_entries`, 2026-09-06)

**What viper_final did.** The intra-line coalescing structure is the
per-way seen-set, `ViperFinal::ConsumerGroup::lineSeen`
(`src/mem/cache/prefetch/viper_final.hh`): folded index keys
(`preLaneKey()`, one per distinct target line) that the lane loop in
`convertChunk()` (`src/mem/cache/prefetch/viper_final.cc`) compares each
element against, before or after the shift/add lane per
`pre_lane_dedup`. Its scope was exactly one captured index line:
`convertChunk()` called `clearLineState()` whenever a line's first
chunk converted (`cap.nextElem == 0`), and `flushConversion()` called
it when a producer's buffered work was discarded. The keys survived
only across the `conversion_lanes` chunks of one line. Cross-line
redundancy was left to the emission-time structures: the fixed-depth
dedup window (`dedup_buffer_size`, keyed by target line address,
first-emit-wins, no touch-refresh) and the eviction-clocked table
(`evict_dedup_entries`). The per-line scope bought a fixed-size CAM
(at most `blkSize/EEW` keys per way, reset by a broadside valid-bit
clear, RTL `seen_valid[IPT_SETS][CPP_MAX]`), kept the folded key
exact (it names a target line only at a fixed form base), and kept
emission-time drops re-emittable from the next line, because the
seen-set inserts at conversion and never learns what emission did.
At VLEN=512 one gather chunk is one 64 B index line, so that scope is
also the ISA-guaranteed-safe one (elements of one gather sharing a
target line coalesce in the LSQ onto one MSHR).

**What was added.** `seen_set_entries` (`ViperFinalPrefetcher`,
`src/mem/cache/prefetch/Prefetcher.py`; member
`ViperFinal::seenSetEntries`). At the default 0 nothing changes. At
N > 0 the seen-set is a persistent FIFO CAM of N keys per consumer
way that does not clear between index lines:

- `lineSeen` became a `std::deque<Addr>` (oldest key first). The lane
  loop still pushes every distinct converted key; when the CAM holds
  more than N keys the oldest is popped (`seenSetReplaced`). The
  compare is unchanged: `std::find` over the keys, hits counted in
  `targetsDeduplicated` in either `pre_lane_dedup` placement.
- `clearLineState()` skips the seen-set at N > 0 (it still resets the
  window contribution `pendingEmitted`), so the first chunk of a new
  line converts against the keys of every earlier line still in the
  CAM.
- Three re-arm events empty the CAM, because its keys stop naming
  target lines: (1) a form push whose operands differ from the way's
  current form, `installForm()` -> `clearWaySeenSet()` (a re-push of
  the same gather keeps the keys only when `sameKeying()` finds
  base/shift/rshift/extension identical; a free way taken by a
  different gather, or a newly pushed way, always clears); (2) a
  producer flush, `flushConversion()` -> `clearSeenSets()` (last way
  invalidated, or a global invalidate); (3) a backward demand jump on
  the producer's index stream, the same site that clears the dedup
  window in `calculatePrefetch()`. Events (1) and (3) are guarded by
  `seenSetEntries != 0` so the default stays bit-identical to the
  Sep 5 binary (at the per-line scope a mid-line clear would have
  changed behaviour).

**How it interacts with the dedup window.** The window still never
sees the line in progress and still records only targets that were
actually pushed; the persistent seen-set now catches cross-line
duplicates first, at conversion, so the window's `targetsCrossDeduplicated`
falls to whatever the CAM's FIFO horizon lets through. The two differ
in what they suppress: the CAM holds every converted key regardless
of emission outcome, so at N > 0 a target dropped by `drop_on_full`,
`drop_batch_confidence`, or `evict_dedup_entries` cannot re-emit until
its key ages out of the CAM (the "a later duplicate may re-emit"
comments at those sites now hold only across that horizon). Every
cross-line suppression is a residency bet, as with the window; the
CAM has no touch-refresh either (a hit does not move the key), so a
line re-referenced every index line still ages out after N distinct
keys and is re-emitted.

**Hardware.** Per way, an N-entry key CAM with a FIFO write pointer
replaces the `MAX_ELEMS`-entry CAM with a valid vector. The broadside
clear survives for the three re-arm events. At `pre_lane_dedup`
the CAM sits before the lanes as before, so a persistent hit still
spends no `conversion_lanes` slot.

**Not measured.** No run yet. To try it:
`--pf-param seen_set_entries=64` (or another N) with the usual
viper_final flags; compare `targetsDeduplicated`,
`targetsCrossDeduplicated`, `seenSetReplaced`, and `pfHitInCache`.

**Not changed.** `ViperRtl` and `~/viper_rtl` keep the per-line
`seen_valid`/`seen_key` arrays; a gem5 run with `seen_set_entries > 0`
has no RTL counterpart.

**Same day, `seen_set_dedup` (Bool, default True).** An ablation knob
to turn the seen-set off entirely: at False the lane loop neither
compares nor inserts keys, so every element fires and every converted
target reaches the latch, `targetsDeduplicated` reads 0, and the
emission-side structures see the full duplicate stream. Within one
source line only the eviction-clocked table can then catch a
duplicate (the first copy is emitted and inserted, the rest hit
`evictDedupSuppress`), since the dedup window never scans the line in
progress. Added for the user's request to isolate the eviction table's
effect on spmv with the intra-line dedup removed.

**Measurement (2026-09-06, spmv poisson3Db, Sep 2 centre, local
docker on the same-day binary;
`runs/viper_final_seenoff/runs/seenoff_ed64_rivec_spmv`).** One run,
`seen_set_dedup=false evict_dedup_entries=64`, against the Sep 2
centre (`ed0`, seen-set on) and the Sep 1 `vf_ed64` leg (seen-set on):

| stat | centre ed0 | seen on, ed64 | seen OFF, ed64 |
|---|---|---|---|
| numCycles | 12,897,759 | 12,899,725 | 12,899,723 |
| dram.readBursts | 1,027,111 | 1,027,115 | 1,027,116 |
| pfIssued | 2,745,922 | 2,442,218 | 2,442,843 |
| pfHitInCache | 577,036 | 277,510 | 278,015 |
| targetsGenerated | 2,196,715 | 1,832,735 | 1,833,560 |
| targetsDeduplicated | 176,789 | 176,782 | 0 |
| targetsEvictDeduplicated | 0 | 364,179 | 541,096 |
| evictDedupReplaced | 0 | 1,801,722 | 1,802,525 |

With the seen-set off, the eviction table's suppressions rose by
176,917, matching the 176,782 the seen-set used to take to within
0.1 %: an intra-line duplicate's first copy is emitted and inserted in
the same latch drain, so the rest hit the table. Only 825 more targets
went out (first copies dropped at emission, or replaced from the
64-entry table before the duplicate arrived) and cycles differ by 2.
On this kernel and config the eviction table subsumes the intra-line
seen-set entirely; the seen-set's remaining value is structural (no
residency bet, no global table, and at `conversion_lanes > 0` with
`pre_lane_dedup` it saves lane slots the emission-side table cannot).
The eviction table's own isolated effect versus the centre is
unchanged from the Sep 1 sweep: pfHitInCache −52 %, pfIssued −11 %,
cycles and DRAM reads flat.

## VectorChainTable: vrsub folds as a negated index (2026-09-11)

**What gem5/this fork did before.** `VectorChainTable::decodeTransform()`
(`src/cpu/vector_chain_table.cc`) tagged only the affine OP-V ops the
walk can fold (`vadd/vsub.vx|vi`, `vsll/vsrl/vsra`, `vmul.vx`, the widening
multiplies, `vsext/vzext`); everything else returned `VOp::Invalid`, so a
tagged index value flowing into it broke the chain (`chainsBroken`).
`vrsub` (funct6 `000011`, `c - v`) was in that breaker set, documented as
"a negated scale". `foldChain()` represented a chain as
`addr = base + (ext(raw) >> rshift) << shift` with only a positive index
term, and it rejected any extension that came after an add/sub
(`if (scaled || extended || out.rshift) return false`).

**Why it mattered.** TSVC `s4114_modified` (`tsvc-gem5/src/tsvc.c`) forms
its gather index as `(LEN_1D-1) - k` with `vrsub.vx` at e32, then
`vsext.vf2 → vsll.vi 2 → vluxei64`. In the Sep 9 campaign viper_final
installed **zero** forms on it (`formsInstalled 0`, `chainsBroken 32000`
per ROI) and fell back to its stream walk, giving 1.04x where the sibling
kernels s4112/s4115 got 1.19x.

**What changed.**
- `VOp::Rsub` added (`vector_chain_table.hh`), decoded from OPIVI/OPIVX
  funct6 `0x03` (`.vi` immediate or `.vx` scalar; the `.vx` operand rides
  the existing `wantsScalar`/`captureScalar` issue-time snoop).
- `FoldedForm::negate`: the index term is subtracted instead of added.
  `foldChain()` folds `Rsub` as `bias = c - bias; negate = !negate`
  (shifts and multiplies distribute over the negation, so the existing
  shift/bias handling is untouched; bias wraps in uint64 like the lane's
  adder).
- `foldChain()` now allows `vsext` after a narrow-width add/sub/rsub when
  no shift has been applied yet: `sext(c ± x) == c ± sext(x)` unless the
  narrow arithmetic wrapped, which index chains do not do in practice.
  `vzext` stays strict (not equal for negative intermediates), and
  anything already shifted stays strict.
- The lane (`ViperFinal::applyForm`, `preLaneKey`,
  `src/mem/cache/prefetch/viper_final.cc`) honours `negate`: one
  add/subtract mux in the adder path, no extra width.
- `ViperRtl::installForm` (`viper_rtl.cc`) skips negated forms: the
  SystemVerilog lane has no subtract path, so such gathers stay
  stream-only in the RTL co-sim until the RTL grows the mux.

## VIPER-FINAL: cross-page translation kick (2026-09-11)

**What gem5 did.** `Queued::getPacket()` is the only place stock gem5
starts the translations of prefetches parked in `pfqMissingTranslation`
(`src/mem/cache/prefetch/queued.cc`); the cache calls it only when
`nextPrefetchReadyTime()` says a ready entry exists, i.e. when `pfq` is
non-empty.

**The wedge.** When every candidate a prefetcher emits is on another
page than the demand access, `pfq` never gets a ready entry, the cache
never pulls, the translation queue (`max_prefetch_requests_with_pending_translation`,
32) fills, and further inserts evict its entries forever
(`pfRemovedFull`). Nothing is ever issued. ReVeLA hit exactly this on
Aug 12 and got a kick (`processMissingTranslations()` made protected
and called from its drain/notify); viper_final did not. It surfaced in
the Sep 9 campaign's VLEN=8192 spmv leg: lookahead
`prefetch_distance(4) x vlen_bytes(1024)` = one full page, so all 635K
stream candidates were cross-page, `pfIssued 0`, cycles identical to
base. At VLEN=4096 the 2 KB lookahead keeps half the candidates on the
demand page, which masked it.

**What changed.** `ViperFinal::drainTick()` and `ViperFinal::notify()`
call `processMissingTranslations(queueSize - pfq.size())` after their
inserts whenever `pfqMissingTranslation` is non-empty, the same kick
ReVeLA has. No parameter; bit-neutral whenever pfq is non-empty
(translations were already started by the pull).

**Era note.** Both changes alter viper_final behaviour, so results on
the rebuilt binary are a new era. The Sep 9 campaign keeps its
original binary for every row; only the two affected cells
(s4114_modified viper, spmv viper VLEN=8192) are re-measured on the new
binary and marked as such in `viper_final_eval_results.xlsx`.

## VIPER-FINAL: strided (`vlse`) index producers (2026-09-12)

**What this fork had.** The CPU side already classified a `vlse` as
`VecMemInfo::StridedLoad`, captured its byte stride at commit
(`VectorChainTable::captureStride`) and exported it through
`ProducerInfo::stride`/`vl` (`src/cpu/vector_chain_table.{hh,cc}`,
2026-08-29 work). `viper_final` rejected every such producer with one
guard in `calculatePrefetch` (2026-09-01): a `vlse` reaches the cache
as one EEW-byte access per element, and viper's chunk model ("one
line-sized access per chunk", walk window sized from the access) would
have treated each element as its own chunk. The research viper's span
model was deleted with that build on 2026-09-04; only its design record
survived. sssp, the one campaign kernel with a `vlse` index producer,
therefore never got a gather prefetch.

**What was added** (`src/mem/cache/prefetch/viper_final.{hh,cc}`, all
behind `info.stride != 0` or a nonzero slot mask, so unit-stride
behaviour is unchanged; `strided_producers=false` restores the
rejection).

- **Admission.** A strided producer is accepted when the stride is a
  positive multiple of the element width and the width divides the
  line (`stridedUnsupported` otherwise): then every element occupies
  exactly one EEW-wide slot of the line it sits in.
- **Span chunking** (`SttEntry::stride/strideElemBytes/stridePhase/
  chunkStart/chunkEnd`). A chunk is `[chunkStart, chunkStart + vl·stride)`.
  An element access inside the span, or up to one span behind its
  recorded start (element micro-ops can complete out of order), is the
  same chunk; an earlier element only moves the start back on the same
  grid. Anything else opens a new chunk (`stridedChunks`); a backward
  jump restarts the walk as for unit-stride.
- **Walk** (`streamAheadStrided`). The window keeps the unit-stride
  lookahead in *bytes of index data*: `prefetch_distance × (vlenBytes/EEW)`
  elements ahead of the chunk start, `vl` elements wide, scaled by the
  stride into a span. It emits the lines the span touches above the
  high-water mark: every line when the stride is below a line, one
  line per element otherwise. `limit_gate` extents are `vl·EEW` bytes,
  not `vl·stride`, so the announced limit is not applied to strided
  walks (not covered, as in the 2026-08-29 design).
- **Chain-only trigger.** Only a strided head that feeds a gather
  streams (`stridedWalkGated` counts the rest). This is the 2026-08-29
  lesson: walking every `vlse` (sssp's weight load included) prefetched
  the wrong rows and lost 2%. The proof used is a live IPT form for the
  producer (`producerLive`), or the chain table's link bit. The link
  bit alone was tried first and gated every chunk on sssp: the DCT is
  small (8–16 rows) and clears every strip, so at element-access time
  the head row has been re-inserted and the strip's gather has not yet
  re-linked it, while the form the previous strip pushed persists.
- **Slot masks** (`IrtEntry::slotMask`, `CapturedLine::slotMask`,
  `strideSlotMask`). Registered at the stream-window departure in
  `getPacket` and carried by `notifyFill` into the captured line: bit
  `k` is set when slot `k` (byte `k·EEW`) holds an element, i.e. for
  the offsets `phase_L + j·stride < 64` with
  `phase_L = (chunkStart − line) mod stride`. `convertChunk` skips
  off-mask slots; they fire no lane and spend no budget. A mask of 0
  means "every slot" (unit-stride lines).
- **Read-hit capture** (`captureStridedReadHit`). The hitting element
  access carries EEW bytes, so the resident line is read off the cache
  (`CacheAccessor::readLine`, the path the scalar chain used) and
  latched under the mask; the registration retires immediately so the
  other `vl−1` element hits on the line are recognised as recaptures.
- Stats: `stridedChunks`, `stridedHitsCaptured`, `stridedUnsupported`,
  `stridedWalkGated`. The RTL model (`viper_rtl`) is a separate class
  and is untouched; its lane has no slot select yet.

**Not covered.** Two strided producers sharing lines that both feed
gathers (the IRT keeps one producer per line); a short final strip
whose next row's first accesses fall inside the previous span (its
stream trigger is late, its captures are not); `limit_gate` on
strided walks.

**Validation** (2026-09-12, `runs/strided_val/`). Unit-stride
neutrality: cc_vector on amazon0302, new binary vs the pre-patch one,
every counter identical (12,450,397 cycles, 846,467 issued). Strided
path on sssp_vector, amazon0302 weighted `.gug` (`bfs2gug -w 1`,
checksum 109482824 as the host golden), era-3 configuration (VLEN 4096,
8 lanes, pd1, base 189,118,815 cycles): the pipeline runs end to end —
873K span chunks, the weight `vlse` gated as unlinked (430K), 381K walk
candidates, 378K fills captured, 2.2M targets from 378K lines (~5.9 per
line, i.e. the 8 masked slots minus dedup: the slot mask selects the
vertex ids, not the weights), checksum unchanged. Performance is
**0.98x**: 2.6M issued, 34K useful, 2.5M unused, DRAM reads 2.04M ->
3.68M. Cause is structural, not the mechanism: delta-stepping visits
rows in FRONTIER order, and amazon0302 rows are under one line long, so
"the span 1 KB ahead in the edge array" is some other vertex's row that
is not about to be relaxed. The 2026-08-29 research build got its 1.17x
on sssp only with the scalar chain (`frontier[i] -> off[] -> row`)
supplying the next row's address; viper_final deleted that subsystem.
The strided path pays off on kernels whose `vlse` stream is sequential
in memory (AoS arrays walked in order); none of the campaign kernels
is one.

## VIPER-FINAL: `prefetch_distance` in cache lines (2026-09-12)

**What this fork had.** `ViperFinalPrefetcher::prefetch_distance` was
denominated in whole-VLEN chunks (`vlen/8` bytes each). In
`src/mem/cache/prefetch/viper_final.cc` the unit-stride walk placed its
window at `addr + prefetch_distance * vlenBytes` (`streamAhead`), the
strided walk looked `prefetch_distance * vlenBytes/EEW` elements ahead
(`streamAheadStrided`), and the strided span model fell back to
`vlenBytes/EEW` elements when the DCT row had no `vl` yet. `vlenBytes`
came from a `vlen` parameter on the prefetcher
(`src/mem/cache/prefetch/Prefetcher.py`), which
`rvv/riscv-rvv-se-ara-prefetcher.py` auto-wired from `--vlen` unless
`--pf-param vlen=` overrode it. Four consequences surfaced in the Sep 9
campaign and its follow-ups:

1. The byte lookahead scaled with VLEN, so the VLEN sweep confounded
   the vector length with the lookahead (pd=4 was 64 B at VLEN 128 and
   4 KB at VLEN 8192; the 4 KB point is a full page and hit the
   cross-page translation wedge fixed on 2026-09-11).
2. The Sep 11 `prefetch_distance` sweep showed the VLEN "knee" was a
   lookahead-in-bytes artefact (pd1 at VLEN 4096 1.967x vs pd4 1.734x).
3. Holding the lookahead constant in bytes across VLENs (the Sep 12
   128 B and 512 B sweeps) required decoupling the prefetcher's `vlen`
   from the core's (`--pf-param vlen=1024 prefetch_distance=1` for
   128 B), a workaround that misused a "hardware VLEN" parameter as a
   unit of measure.
4. Any distance below one chunk was unrepresentable at wide VLENs.

**What changed** (user request 2026-09-12). `prefetch_distance` now
counts cache lines and the prefetcher carries no VLEN at all:

- `viper_final.cc`: `from = addr + prefetchDistance * blkSize`
  (`streamAhead`); `elems_ahead = prefetchDistance * blkSize / EEW`
  (`streamAheadStrided`); the strided `vl` fallback is `blkSize / EEW`
  (one line of elements; reached only if an element access precedes
  the DCT row's `vl` capture, which `VectorChainTable` performs at the
  producer's dispatch). The `vlenBytes` member and the
  `vlen`-must-be-set check are deleted; `prefetch_distance >= 1` is
  kept (0 lines would walk the demand line itself).
- `Prefetcher.py`: the `vlen` parameter is removed from
  `ViperFinalPrefetcher`; `prefetch_distance` defaults to 8 lines
  (512 B, the measured spmv sweet spot at the 64-entry queue / 16
  MSHR sizing).
- `rvv/riscv-rvv-se-ara-prefetcher.py`: the `--vlen` auto-wire now
  applies only to `viper_rtl`, whose Verilated model still uses the
  chunk unit (`~/viper_rtl` is untouched, so `viper_rtl`'s
  `prefetch_distance` and `viper_final`'s are no longer the same
  quantity; the RTL diff testbench must be driven with byte-equal
  distances).

**Equivalence to the recorded runs.** For a chunk-era cell
`(pd_chunks, VLEN)` the same window is
`prefetch_distance_lines = pd_chunks * VLEN/8 / 64` whenever that is a
whole number, and `elems_ahead` is identical because both are
`bytes / EEW`. The campaign centres map as: era-1 centre pd4 @ VLEN
4096 = 32 lines; era-3 centre pd1 @ VLEN 4096 = 8 lines; the fixed
128 B / 512 B legs = 2 / 8 lines; the VLEN sweep at pd4 = 1/4/8/16/32/64
lines for VLEN 128/512/1024/2048/4096/8192; the pd1 VLEN legs =
0.25/1/2/4/8/16 lines, of which VLEN 128 (16 B) is not representable
and rounds up to one line. Results on the rebuilt binary are expected
bit-identical to the `vlen`-workaround runs at equal byte distances,
but this is unverified until the binary is rebuilt (the current
`build/RISCV/gem5.opt`, md5 055d7afd..., predates this change); viper
rows measured on it are a new era.

**Drivers.** `rvv/sweeps/viper_final_eval_sweep.py`,
`viper_final_eval_pd1_sweep.py` and `viper_final_ed_sweep.py` no
longer pass `vlen=` and express `prefetch_distance` in lines, with a
`chunk_lines()` helper mapping the recorded chunk-era legs to the same
byte lookahead (so re-running any DONE-less cell reproduces the
recorded configuration up to the VLEN 128 @ pd1 rounding above).

**Storage.** None: one parameter removed, no state added.

## VIPER-FINAL: segmented (`vlseg`) index producers and way/field IPT ways (2026-09-16)

**What this fork had.** A unit-stride segment load (`vlseg<nf>e<eew>`)
was invisible to the whole chain. gem5 splits it into memory micros
(`VlSegMicroInst`, `src/arch/riscv/isa/templates/vector_mem.isa`
`VlSegMicroInitiateAcc`) that each issue ONE contiguous request of
`microVl * EEW/8` bytes into an internal `vtmp` register
(`EA = rs1 + mem_size * (microIdx + field * numMicroops)`,
`formats/vector_mem.isa`), followed by arithmetic de-interleave micros
(`VlSegDeIntrlvMicroInst`, `src/arch/riscv/insts/vector.{hh,cc}`) that
read every `vtmp` and write one field's architectural `vd`. Neither
class overrode `vecMemInfo()`, so the memory micro fell into
`VectorChainTable::dispatch`'s "any other memory op: invalidate" branch
(`src/cpu/vector_chain_table.cc`) and the de-interleave copied a cleared
provenance slot: no head, no PT tag, no gather link. TSVC `seg1`/`seg2`
(added 2026-09-16, `tsvc-gem5/src/tsvc.c`: `vlseg2e32 -> vsext.vf2 ->
vsll.vi -> vluxei64`, one and two consumer gathers) streamed nothing.
Also, an IPT way knew only the gather's folded form; two gathers on two
different fields of one producer were indistinguishable from two gathers
on the same index vector (`multi_way`).

**What was added.** Three pieces, none touching unit-stride behaviour
(verified bit-identical on `s4112`/`multi_way`, see below).

1. *ISA reporting* (`src/cpu/static_inst.hh`, `VecMemInfo`): two kinds.
   `SegmentLoad` on the memory micro (`VlSegMicroDeclare` template):
   `elemBytes = EEW/8`, `nfields = nf`, `microIdx` = the chunk index
   (0 = the macro's base access), `vl`. `SegmentField` on the
   de-interleave micro: `field` (0..nf-1) and `srcVReg` = its first
   `vtmp` source. The memory micro's own `field` member is a chunk
   index, not a data field; data fields are born at the de-interleave.

2. *Chain table* (`src/cpu/vector_chain_table.{hh,cc}`):
   - `PtEntry::field` (3 bits). Written by the `SegmentField` branch of
     `dispatch()`: `pt[vd] = pt[srcVReg]` with the field stamped. Carried
     through every transform (`PtEntry{true, idx, dep_field}`, read
     before the destination write so in-place ops keep it) and every
     plumbing copy (`VCpyVs` copies the whole entry).
   - `DctEntry::nfields` (head, from `SegmentLoad`; every field micro
     carries the macro PC, so `searchPc` lands them on one row),
     `DctEntry::segBase/segBaseValid` (head, see 3), `DctEntry::field`
     (gather row, `= pt[srcVReg].field` at backprop, re-anchored with
     `lastDctPtr`).
   - `FoldedForm::field`: set from the gather row when the walk pushes;
     `FormSink::installForm` gains `nfields` (per producer, like
     `elem_bytes`). `ProducerInfo` exports `nfields`, `segBase`.
   - `captureSegBase(pc, base)`: called from `src/cpu/o3/commit.cc` at
     commit of the chunk-0 `SegmentLoad` micro with `effAddr` (loads
     carry their VA at commit; no issue-time snoop). It is the phase
     reference of the slot masks and is refreshed every strip; the
     phase `base mod (nf*EEW)` is what matters and it does not move.
   - Stats: `segmentHeadsAllocated`, `segBasesCaptured`,
     `segmentFieldsTagged`.

3. *Prefetcher* (`src/mem/cache/prefetch/viper_final.{hh,cc}`):
   - Memory-side a `vlseg` IS a unit-stride stream, so chunk detection,
     the walk, IRT registration and fill capture are the unit-stride
     path unchanged. Admission (`calculatePrefetch`): `segment_producers`
     true, EEW dividing the line, <= 64 slots (`segmentUnsupported`
     otherwise). The STT entry mirrors `nfields`, `elemBytes`, `segBase`
     from `ProducerInfo` on every access.
   - `IptSet::nfields` (per producer, beside `elemBytes`, written at
     every `installForm`).
   - `captureSlotMask(ps, line)`: the mask a captured line carries.
     Strided -> the vlse grid mask as before; segment ->
     `segmentSlotMask(line, EEW, nf, base)`: the FIELD-0 BASE MASK in
     closed form, `PATTERN[nf] << phase_slot`, `PATTERN[nf]` a constant
     with one bit every nf slots and `phase_slot = ((base - line) mod
     (nf*EEW)) / EEW`. When the stride divides the line (nf 1/2/4/8)
     the phase is the low bits of the base, constant per producer; for
     nf 3/5/6/7 it is a small constant-modulo per line. Wired into the
     `getPacket` registration and the read-hit capture; the fill path
     already copied the IRT record's mask. Without a captured base the
     grid is assumed line-aligned (`segmentPhaseUnknown`).
   - `fieldSlotMask(base, nf, f)`: a way's mask = the base mask rotated
     by its field id within the nf-slot period,
     `(base << f) | (base >> (nf - f))`. Derived in `convertChunk` at
     the head of the ILQ from the line's base mask and the way's
     `FoldedForm::field`; NEVER stored per way (3 bits per way replace
     64). The element loop gained one per-way bit test: a slot outside
     a way's field gates that way's lane set (`fieldSlotsSkipped`,
     no lane, no budget), and a slot no live way wants is skipped
     before the broadcast like an off-grid vlse slot (`any_mask`).
     The any-miss slot rule is unchanged, so `seg2` (fields 0 and 1)
     costs the same lane budget per line as `s4112`.
   - Ways are now way/fields: `consumers_per_producer` default 2 -> 4
     (`Prefetcher.py`; the config scripts copy it onto the chain
     table's `consumers_per_producer`). Bit-neutral for producers with
     <= 2 gathers; kernels with 3-4 gathers on one producer (`s353`)
     now install 4 forms.
   - `ConvertedLine::next` widened `uint8_t -> uint16_t`: 64 e8 slots x
     4 ways = 256 targets overflowed 8 bits.
   - `viper_rtl` (`src/mem/cache/prefetch/viper_rtl.cc`): signature
     updated; segment forms are dropped there (stream-only in the RTL
     co-sim, like negated forms) — the RTL port itself is unchanged.

**Target generation on the three TSVC shapes** (way = (field -> base)):

| kernel | ways | slots 0,1,2,3... |
|---|---|---|
| `multi_way` | (0 -> b), (0 -> c), nf 1 | both ways every slot |
| `seg1` | (0 -> b), nf 2 | way 0 on even slots; odd slots free |
| `seg2` | (0 -> b), (1 -> c), nf 2 | way 0 even, way 1 odd; one slot each |

**Storage.** +3 bits per PT slot (40), +3 bits per DCT row (field) and
+3 bits + 1 VA per head row (`nfields`, `segBase`), +3 bits per IPT set
(`nfields`), +3 bits per IPT way (`FoldedForm::field`), +1 bit per latch
cursor. No per-way mask storage; the per-line 64-bit mask already
existed for `vlse`.

**Intra-macro fragment reordering (found in verification).** A vlseg
macro is nf memory micros of `vl*EEW` bytes back to back, and the LSQ
issues their line fragments out of order. The unit-stride chunk
detector in `calculatePrefetch` treated every access behind the cursor
as a restarted walk (`limitAddr = 0`), and a queued index line beyond
the shrunken high-water mark then failed the registration test in
`getPacket` at issue: on `seg1` (MEDIUM) only 8,644 of 16,384 index
lines registered, coverage 0.50. For segment producers a step back
that stays inside the macro span (`vl x nf x EEW` bytes) is now
treated as that reordering: no reset, the cursor keeps its higher
position (the capture window is measured from it), and the walk from
the earlier address lands below the mark and emits nothing
(`segmentReorderTolerated`; `walkRestarts` counts the resets that still
happen). Unit-stride producers keep the plain rule, so `s4112` and
`multi_way` stay bit-identical.

**Per-fragment pushes without the high-water filter (found in
verification).** The reorder tolerance alone left registrations at
half. The `--debug-flags=ViperFinal` trace (`chunk:` / `issue:` lines
added 2026-09-16, `runs/seg_verify/tiny_seg1/`) showed why: the two
memory micros' 64 B fragments interleave at the cache (A, A+512, A+64,
A+576, ...). The unit-stride walk pushes one line per fragment and
drops any line below the monotone high-water mark as already walked;
micro 1's first fragment raised the mark 512 B past micro 0's
remaining targets, so every line in between was never pushed, issued,
registered or converted: exactly half. A first fix walked the whole
next-macro window (16 lines) on each macro's first fragment; it
restored coverage but delivered the 16 fills in one burst that
overflowed the 8-entry index line queue (`bufferBusyDrops` 1,869 on
seg1, 5,466 on seg2). The shipped shape keeps the per-fragment
one-line push of a `vle` and, for nf >= 2 only, skips the high-water
filter in `streamAhead`: each fragment maps to exactly one distinct
line by its offset in the macro, so the filter's premise (in-order
accesses) is what fails, not the push. A replayed fragment re-pushes
a line the queue filter or the resident-line check drops. The mark is
still raised because `getPacket` bounds the capture window with it.
`streamAhead` also scales the frontier by nf (`prefetch_distance x nf`
lines, one macro ahead) so the lookahead in elements per field equals
the unit-stride one, the rule `streamAheadStrided` applies to `vlse`.
`nf <= 1` producers are untouched.

**Verification (2026-09-16, `runs/seg_verify/`, TSVC MEDIUM `-i 1
-synth -S 1 s4112 multi_way seg1 seg2`, campaign core + era-3 viper
config, local docker).** `s4112` and `multi_way` are bit-identical to
the era-4 binary on both arms in every counter compared (cycles,
simInsts, DRAM reads, L1D misses, pfIssued/pfUseful, targets), with
`consumers_per_producer` 4 vs 2. On the era-4 binary `seg1`/`seg2`
installed no forms (stream-only, ~1.01x). Final build (45c6d0f21e2b,
per-fragment pushes), viper over base, MEDIUM: `seg1` 1.303x (accuracy
0.82, coverage 0.64, 15,979 of 16,384 index lines registered and 15,440
captured, `bufferBusyDrops` 539 versus 1,869 on the macro-walk build;
one form per gather, the macro base captured every strip, field masks
`0x5555`), `seg2` 1.165x (accuracy 0.91, coverage 0.57;
`fieldSlotsSkipped == elementsConverted`, i.e. every converted slot
fired exactly one of the two field ways; targets for both `b[]` and
`c[]`). seg2's remaining 4,397 queue drops are the stall-gated drain
(`drain_floor=0`) holding lines while the prefetch queue is full
(`pfRemovedFull` ~17k), the same mechanism that drops 1,172 fills on
`multi_way`, not the segment path. Replayed fragments re-push their
line: 1,162 resident-drops on seg2, 58 on seg1.

**Not covered.** Strided segment loads (`vlsseg`) and indexed segment
loads (`vluxseg`) keep their old handling (rejected / invisible). A
producer base that is not EEW-aligned truncates the phase to a slot.
