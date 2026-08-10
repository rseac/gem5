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
  `GatherDescriptorTable`), the v1 dispatch/arming hooks in
  `src/cpu/o3/inst_queue.cc`, and the `gdp_table` BaseCPU param's old
  type. The `StaticInst::gdpInstInfo()` classification virtual
  (static_inst.hh, vector.hh) SURVIVES — the current GDP table uses
  the same channel.
- **VStride** (`--prefetcher vstride`): confidence-free vector stream
  prefetcher driven by v1's unit-stride-producer records. The current
  GDP's architectural stream walk is the same mechanism (its
  `stream_only=true` ablation is VStride, exactly), so the standalone
  prefetcher was redundant. Deleted: `src/mem/cache/prefetch/
  vstride.hh/.cc`, `VStridePrefetcher`, debug flag `VStride`,
  `rvv/vstride_test.c`.

The current-name mapping after the rename: `--prefetcher gdp` is the
transform-chain design, class `GDPPrefetcher` / `gem5::prefetch::GDP`,
CPU sideband `GdpChainTable` on BaseCPU param `gdp_table`, debug flag
`GDP`. Old sweep rows recorded under `gdp_*`/`gdp_sap_*`/`gdp_skip*`
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
  sideband channel, same wiring pattern as `GdpChainTable`)
- `src/mem/cache/prefetch/tyche.hh/.cc` (new — the cache-side
  prefetcher: IPT training/trigger, walk registration, fill capture,
  chain replay)
- `src/mem/cache/prefetch/Prefetcher.py` (additive `TychePrefetcher`)
- `src/cpu/BaseCPU.py`, `src/cpu/base.hh/.cc` (additive `tyche_table`
  param, following `gdp_table`)
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

- `src/cpu/gdp_table.hh/.cc`, `src/cpu/GdpTable.py` (the
  `GdpChainTable` SimObject: PT + DCT, the CPU-to-prefetcher
  sideband, same wiring pattern as `TycheChainTable`)
- `src/mem/cache/prefetch/gdp.hh/.cc` (the cache-side
  prefetcher: architectural stream, range-based capture, replay)
- `src/mem/cache/prefetch/Prefetcher.py` (`GDPPrefetcher`)
- `src/cpu/BaseCPU.py`, `src/cpu/base.hh/.cc` (`gdp_table` param)
- `src/cpu/o3/inst_queue.cc` (additive hooks next to the Tyche
  ones: vector dispatch feed + issue-time base/scalar snooping)
- SConscripts (registration; `GDP` debug flag);
  `rvv/prefetcher_factory.py`, `rvv/vector_cache_hierarchy.py`,
  `rvv/riscv-rvv-se-ara-prefetcher.py` (selection, per-core wiring —
  the gdp param is `link_table`, distinct from tyche's `chain_table`
  so the dual-config `_params` guards stay unambiguous)

**CPU side (`GdpChainTable`), hardware widths in mind:**

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
both paths. Mode 1 (`x[idx[i]]`): expect `gdp_table.linksFormed>=1`,
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
It shares GDP's entire CPU side — the same `GdpChainTable` SimObject,
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
  no new CPU-side code — `GdpChainTable` is reused unmodified)
- `src/mem/cache/prefetch/Prefetcher.py` (`VectorTychePrefetcher`; the
  param is `link_table`, the *same name* GDP uses, so the per-core
  wiring in `rvv/vector_cache_hierarchy.py` — which guards on
  `"link_table" in prefetcher._params` — picks it up with no change)
- SConscripts (registration; `VTyche` debug flag)
- `rvv/prefetcher_factory.py` (`PREFETCHERS`, `VA_PREFETCHERS`,
  `GDP_TABLE_PREFETCHERS`), `rvv/riscv-rvv-se-ara-prefetcher.py`
  (`--prefetcher vtyche`)

### What GDP does

`GDP::calculatePrefetch` (`gdp.cc:300-318`) adopts a
`GdpChainTable::ChainSnapshot` — a *list* of `StageOp{op, scalar,
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
| `link_table` | NULL | the per-core `GdpChainTable` (wired by the config script) |
| `streaming_distance` | 8 | index-array stream lookahead in lines; also sets the indirect lookahead |
| `stream_only` | False | ablation: stream but never capture/convert |
| `address_queue_entries` | 2 | converted batches held per pipeline |
| `pipelines` | 2 | concurrently configured producers |
| `routing_entries` | 32 | IRT capacity |
| `drain_floor` | 8 | minimum emissions per event when the queue is full; 0 = pure stall |

### Behavior changes

None to any existing configuration: this is an additive prefetcher
selection. `--prefetcher vtyche` requires `--prefetcher-side vector`
(same gate as gdp, via `GDP_TABLE_PREFETCHERS`) and is not accepted for
`--scalar-prefetcher`. It trains on virtual addresses, so the CPU MMU
is registered automatically.

### Validation

No new test binary — `rvv/gdp_test.bin` covers both shapes. Run
2026-07-28, vlen 512 / throughput 4 / simd-units 2, prefetcher on the
vector L1:

| | mode 0 (`x[idx[i]+8]`) | mode 1 (`x[idx[i]]`) |
|---|---|---|
| `gdp_table.transformsInserted` | 2 | 1 |
| `gdp_table.linksFormed` | 1 | 1 |
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
  `TycheChainTable` / `GdpChainTable` sideband tables wired to both
  the prefetcher (`chain_table` / `link_table`) and the core
  (`tyche_table` / `gdp_table`);
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
`gdp_table.linksFormed` fires, with scalar accesses passing through
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

## VTyche: self-clocked drain (`drain_period`, 2026-08-04)

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

`VectorTychePrefetcher.drain_period` (Cycles, default 1; `0` restores
the legacy demand-clocked drain). With it set, VTyche self-schedules
an `EventFunctionWrapper` (`drainEvent`, `vector_tyche.cc`): while any
producer holds buffered or half-emitted conversion work, the drain
fires every `drain_period` cycles, converting (still at most one line
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

1. `GdpChainTable` (`src/cpu/gdp_table.{hh,cc}`) grew a stream-page
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
   + gdp-table prefetcher only) wires the policy in
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
gdp-table prefetcher and limits classification to the producer index
arrays the prefetcher streams. To evaluate the policy in isolation —
and to cover demand-fetched lines and store streams — the registry
can now also be fed from the demand path:

1. `StaticInst::GdpInstInfo` (`src/cpu/static_inst.hh`) grew a
   `UnitStrideStore` kind, implemented by a `gdpInstInfo()` override
   on `VseMicroInst` (`src/arch/riscv/insts/vector.hh`) mirroring the
   existing `VleMicroInst` one. `GdpChainTable::dispatch` matches
   kinds exactly, so stores still take no part in chain construction.
2. `GdpChainTable` grew `demand_stream_pages` (Python param, default
   False) and `notifyDemandAccess(si, paddr)`
   (`src/cpu/gdp_table.{hh,cc}`): when enabled, a unit-stride vector
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
   `GdpChainTable` wired to `cpu.core.gdp_table` and the policy's
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

1. `GdpChainTable::promoteStreamPage(pa)` removes the page from the
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
