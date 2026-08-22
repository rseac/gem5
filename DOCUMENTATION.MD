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
  technique — no ISA edits): vadd/vsub/vrsub, vand/vor/vxor,
  vsll/vsrl/vsra, vmul.vx, vsext/vzext (source width from the vtype
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
  trio is `scalar` / `immType` (true = encoding immediate, false =
  .vx rs1 snooped at issue) / `immValid` (operand has arrived — set
  at insert for immediates, captureScalar for .vx, armBase for
  bases). `lastDctPtr` is 3 bits in hardware: heads have no
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
extFromBits}` in head-to-gather order — into the producer's STT entry.
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
