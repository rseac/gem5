# Scalar Cache Bypass

Marks every **non-vector** data access `Request::UNCACHEABLE` so L1/L2
forward it straight to memory without allocating, leaving cache contents,
MSHR behavior, and prefetcher training driven **only by RVV accesses** —
the isolation mode for prefetcher experiments. Off by default; enable with:

```bash
build/RISCV/gem5.opt rvv/riscv-rvv-se-ara-prefetcher.py \
    --enable-chaining --vlen 512 --vector-timing-throughput 4 --simd-units 2 \
    --scalar-uncacheable /path/to/riscv-binary
```

## What gem5 does by default

- The classic cache already implements a complete per-request bypass path,
  keyed off `Request::UNCACHEABLE`: `Cache::access` skips lookup, flushes
  any cached copy of the touched line, and treats the access as a forced
  miss (`src/mem/cache/cache.cc:166-180`); `handleTimingReqMiss` never
  coalesces it and forwards the **original packet** downstream
  (`cache.cc:331-345`, `mshr->isForward`), so the access traverses L1 → L2
  → memory as a pass-through. Prefetchers ignore uncacheable accesses
  (`src/mem/cache/prefetch/base.cc:179`).
- `UNCACHEABLE` and `STRICT_ORDER` are independent request-flag bits
  (`src/mem/request.hh:126,136`). The only code that couples them is the
  RISC-V PMA checker (`src/arch/riscv/pma_checker.cc:69`), which runs
  **only in full-system mode** (`pma->check` call at
  `src/arch/riscv/tlb.cc:579`, inside the `if (FullSystem)` branch at
  `tlb.cc:512`). SE-mode translation goes through the process page table
  and never sets either flag.
- The O3 LSQ serializes accesses at the ROB head only when
  `isStrictlyOrdered()` (`src/cpu/o3/lsq.cc:855`,
  `src/cpu/o3/lsq_unit.cc:511,568,1400`); it never inspects
  `isUncacheable()` (the only O3 use is an icache-response assert,
  `src/cpu/o3/fetch.cc:1662`). **Therefore, in SE mode, an
  UNCACHEABLE-only access stays speculative and out-of-order** — it just
  always goes to memory.

## New Features

| Change | File |
|---|---|
| `scalar_uncacheable` parameter (default `False`) | `src/cpu/BaseCPU.py:104` |
| `scalarUncacheable` member, initialized from the param | `src/cpu/base.hh:430`, `src/cpu/base.cc:140` |
| Tagging at request creation: non-vector, non-LLSC, non-AMO requests get `Request::UNCACHEABLE` | `src/cpu/o3/lsq.cc:1115` (in `LSQRequest::addReq`, next to the `annotateMemRequest` hook) |
| `--scalar-uncacheable` CLI flag, plumbed through `RVVCore` to `core.scalar_uncacheable`, shown in the config banner | `rvv/riscv-rvv-se-ara-prefetcher.py` |

The classification is `DynInst::isVector()` (the `IsVector` static-inst
flag), so all RVV loads/stores — unit-stride, strided, indexed, segmented —
stay cacheable; everything else (integer/FP scalar loads and stores)
bypasses. Atomics and LR/SC are excluded from tagging
(`isLLSC`/`isAtomicReturn`/`isAtomicNoReturn`) because uncacheable LLSC
semantics are untested in the classic memory model. Instruction fetch never
passes through `addReq`, so the L1I is unaffected.

In the debug trace, bypassed accesses are directly visible: `Packet::print`
already shows ` UC` for uncacheable requests, which now composes with the
RVVExtension tag:

```
...l1d-cache-0: access for ReadReq [27e10:27e17] type=MemRead UC
```

## Caveats (read before interpreting results)

1. **Flush-on-collision.** An uncacheable access to a line currently cached
   evicts it first (`cache.cc:175-179`) — a scalar read of vector-written
   data kicks that line out of L1/L2 (dirty lines are written back to
   memory first). Keep
   scalar and vector data on disjoint cache lines inside the ROI for clean
   isolation; scalar checksum loops after the ROI are harmless.
2. **Cycle counts are not faithful; cache/prefetcher characterization is.**
   Every scalar access pays the full memory round trip, so absolute
   IPC/cycle numbers are pessimistic for scalar-heavy phases. Hit/miss
   rates, prefetcher accuracy/coverage/timeliness, and the vector access
   stream are the intended use.
3. **SE mode only.** In FS mode the PMA checker owns this flag (and pairs
   it with `STRICT_ORDER`); this feature is designed for the SE-mode ARA
   experiments.
4. **O3 only.** The tagging lives in the O3 LSQ; AraMinor runs ignore the
   flag (the param exists on BaseCPU but nothing reads it outside O3's
   `addReq`).
5. **Uncacheable stores drain slower.** Each scalar store occupies its SQ
   entry until memory responds (`handleUncacheableWriteResp`,
   `src/mem/cache/base.cc:562`), so store-heavy scalar phases add SQ
   pressure. Correct, just pessimistic.

# Parallel Vector Cache Hierarchy (`VectorSplitter`)

Gives vector memory accesses their own private L1D+L2 chain alongside the
scalar caches, replacing the `--scalar-uncacheable` approach for isolating
the vector access stream. Unlike UNCACHEABLE tagging, both paths stay
cacheable and coherent, so lines touched by both access classes (vectorized
newlib's `_malloc_r` headers, `printf` stack frames — see the caveats of the
previous section) migrate between the hierarchies via ordinary snooping
instead of silently diverging.

## What gem5 provides, and why none of it steers by instruction type

Every routing component in the classic memory system picks a downstream
port **by address**: `BaseXBar::findPort` resolves an `AddrRangeMap`
(`src/mem/xbar.cc:334`, map at `src/mem/xbar.hh:319`), and `Bridge`/
`AddrMapper` are address-based 1-in-1-out adapters. The O3 CPU has exactly
one data port (`LSQ::DcachePort`, instantiated at `src/cpu/o3/lsq.hh:984`);
every load/store leaves through `LSQUnit::trySendPacket`
(`src/cpu/o3/lsq_unit.cc:1267`). There is no stock object that routes a
packet by what *instruction* produced it.

The one component with the right *shape* is `CommMonitor`
(`src/mem/comm_monitor.hh`): a zero-latency pass-through SimObject with one
CPU-side `ResponsePort` and one mem-side `RequestPort` that forwards every
protocol action (timing/atomic/functional, both snoop directions, retries)
in the same call chain. `VectorSplitter` is that pattern with **two**
mem-side ports and one steering decision.

## New Features

### 1. `isVector` on `RVVExtension` (`src/mem/rvv_ext.hh`)

The extension already carried the OpClass; classifying "is this a vector
access" from OpClass would mean hardcoding the full list of `Simd*` memory
classes. Instead the extension now also carries the `StaticInst::isVector()`
flag — the same authoritative bit the `--scalar-uncacheable` tagging uses
(`src/cpu/o3/lsq.cc:1115`), set by the decoder on every RVV inst class
(`src/arch/riscv/insts/vector.hh:97` etc.,
`src/arch/riscv/isa/templates/vector_mem.isa:337`).

The constructor takes all three fields with **no defaults**
(`RVVExtension(OpClass, int64_t rs2, bool is_vector)`) so any new
annotation path must state the vector flag explicitly — a stale call site
is a compile error, not a silently mis-steered request. All three existing
construction sites were updated: the base hook (`src/cpu/static_inst.cc:98`)
passes `isVector()`, and the two strided-access overrides
(`VlElementMicroInst`/`VsElementMicroInst::annotateMemRequest`,
`src/arch/riscv/insts/vector.hh`) pass it alongside rs2.

### 2. The `VectorSplitter` SimObject (new files, no core edits)

| File | Content |
|---|---|
| `src/mem/VectorSplitter.py` | SimObject: `cpu_side_port` (ResponsePort), `scalar_side_port` + `vector_side_port` (RequestPorts) |
| `src/mem/vector_splitter.hh/.cc` | CommMonitor-style forwarding with a steering decision in `recvTimingReq`/`recvAtomic`/`recvFunctional`/`tryTiming` |
| `src/mem/SConscript` | registration + `DebugFlag('VectorSplitter')` |

Steering rule (`VectorSplitter::isVectorAccess`): a request goes to the
vector side iff it carries an `RVVExtension` with `isVector() == true`.
Untagged requests (none expected on the O3 dcache path — the LSQ annotates
every request in `LSQ::LSQRequest::addReq`) default to the scalar side and
are counted in the `untaggedReqs` stat as a sanity check.

Protocol handling, relative to `CommMonitor`:

- **Requests** are classified *before* `sendTimingReq` (a successful send
  hands over packet ownership, same reason CommMonitor snapshots
  `pkt_info` first).
- **Request retries**: only the downstream port that refused a packet ever
  sends one, and the O3 LSQ blocks after a refusal
  (`lsq->cacheBlocked(true)` in `trySendPacket`), so at most one is in
  flight; it is forwarded straight up.
- **Response retries**: the splitter keeps a single pending slot
  (`respRetryPort`). This relies on the O3 dcache port always accepting
  responses (`LSQ::DcachePort::recvTimingResp` returns true
  unconditionally); a second simultaneous refusal panics with an explicit
  message rather than corrupting retry state.
- **Snoops** from either downstream hierarchy are forwarded up to the CPU
  (LL/SC tracking, load-order checks). The CPU never *sends* snoop
  responses, so `recvTimingSnoopResp`/`recvRetrySnoopResp` panic — they
  document the assumption that the cpu-side peer is a CPU port, not a
  cache.
- **Functional/atomic** accesses arriving on `cpu_side_port` steer like
  timing requests. Functional accesses from the **system port** (SE-mode
  syscalls reading/writing guest buffers, `m5_write_file`) never traverse
  the splitter: they enter at the membus and reach *both* hierarchies as
  functional snoops, so dirty lines are found wherever they live. This is
  exactly the path that UNCACHEABLE tagging broke.
- **Same-line cross-side serialization.** The two hierarchies are
  independent coherence agents serving one thread, and that breaks
  same-address store ordering: if a program-earlier store *misses* on one
  side while a program-later store to the same line *hits* on the other,
  the later store becomes globally visible first, and the earlier miss's
  line-fill then applies its data on top — a lost update. This is not
  hypothetical: vectorized newlib's `_calloc_r` zeroes a fresh `Bigint`
  with `vse64.v` (vector side) and `_Balloc` writes the `_k`/`_maxwds`
  header fields with scalar `sw` 8 cycles later (scalar side); the zeros
  won, `_maxwds` read back 0, and newlib's `__lshift` looped forever
  (`for (i = _maxwds; n1 > i; i <<= 1)` never terminates from 0). The
  splitter therefore keeps an `outstanding` map (line → side, count) of
  in-flight requests; a request whose line is outstanding on the *other*
  side is refused and retried (`sendRetryReq`) once that line's response
  drains. Cross-side sharing degrades to coherent ping-pong instead of
  corrupting. The `conflictStalls` stat counts these; for cleanly
  separated workloads it stays 0.
- Apart from conflict stalls, forwarding adds **zero latency** (same
  call chain), so with `--vector-cache` off vs. on, a program with no
  cross-side line sharing should be cycle-identical to the baseline
  hierarchy.

### 3. `VectorSplitCacheHierarchy` (`rvv/vector_cache_hierarchy.py`)

Subclasses the stdlib `PrivateL1PrivateL2CacheHierarchy` and overrides only
`incorporate_cache` (the parent's body is replicated since the dcache
connection sits mid-loop; everything else — membus, system port, L1I,
walker ports on the scalar L2XBar, interrupts, IO cache — is unchanged from
`src/python/gem5/components/cachehierarchies/classic/private_l1_private_l2_cache_hierarchy.py:125`).
Per core it adds `vector-l1d-cache-N` → `L2XBar` → `vector-l2-cache-N` →
membus, and connects:

```
cpu.connect_dcache(splitter.cpu_side_port)
splitter.scalar_side_port = l1d.cpu_side
splitter.vector_side_port = vector_l1d.cpu_side
```

Both chains use the same L2XBar-between-levels structure so L1→L2 latency
is symmetric. The vector caches are the same stdlib `L1DCache`/`L2Cache`
classes. `incorporate_cache` now sets `prefetcher = NULL` on **every** cache
it creates (scalar L1I/L1D/L2 and vector L1D/L2), overriding the stdlib
`StridePrefetcher` default, and then attaches an optional, caller-supplied
prefetcher to exactly one vector cache — see
**Selectable Vector Prefetcher** below.

### 4. CLI (`rvv/riscv-rvv-se-ara-prefetcher.py`)

```bash
build/RISCV/gem5.opt rvv/riscv-rvv-se-ara-prefetcher.py \
    --enable-chaining --vlen 512 --vector-timing-throughput 4 --simd-units 2 \
    --vector-cache --vector-l1d 32KiB --vector-l2 512KiB \
    /path/to/riscv-binary
```

`--vector-cache` selects the split hierarchy; `--vector-l1d`/`--vector-l2`
size the vector chain (scalar sizes stay on `-d`/`-2`). Each cache's MSHRs
are also flags: `--{l1d,l2,vector-l1d,vector-l2}-mshrs` (defaults 16/20/
16/20, the stdlib values) bound how many outstanding line misses that
cache overlaps, and `--{l1d,l2,vector-l1d,vector-l2}-tgts-per-mshr`
(defaults 20/12/20/12) bound how many demands coalesce on one outstanding
line (gathers park many same-line element accesses on one MSHR). Lowering
`--vector-l1d-mshrs` (e.g. 4) chokes the miss-level parallelism that hides
gather-element miss latency — a cheap way to make gathers latency-bound
without growing the working set past L2. Without the flag
the original single hierarchy is used, untouched. Combining with
`--scalar-uncacheable` is possible but pointless (scalars would skip their
own hierarchy).

## Caveats

1. **One CPU port remains.** If the vector path blocks (vector L1D MSHRs
   full), scalar requests queued behind it in the LSQ stall too — the
   splitter does not add bandwidth, it only separates cache state. Use the
   second-dcache-port design if port contention itself becomes a variable.
2. **Cross-side line sharing is correct but slow.** Lines written by both
   access classes ping-pong between the hierarchies (snoop supply +
   conflict stalls). Workloads built against vectorized newlib
   (`riscv64-unknown-elf` toolchain) share lines constantly inside
   malloc/printf; expect inflated cycle counts in those phases and keep
   ROI measurements between `m5_reset_stats`/`m5_dump_stats`. Watch
   `conflictStalls` to quantify the effect.
3. **Functional accesses on the dcache port steer by tag.** Fine under the
   no-overlap assumption (and the CPU rarely sends them); system-port
   functional accesses are unaffected (snooped into both chains).
4. **Writebacks/prefetches below the splitter are unaffected** — they
   originate inside each chain and never re-cross the splitter; the
   `untaggedReqs` stat therefore only ever counts CPU-issued requests.
   They are also invisible to the `outstanding` map, which is safe: once
   a store's response has returned it is in the global coherence order,
   and any later access on either side finds it via snooping.
5. **O3/SE focus.** The steering tag is attached by the O3 LSQ; AraMinor
   would send untagged requests (everything lands scalar-side).

# Selectable Vector Prefetcher

Lets you pick and tune a hardware prefetcher from the command line and attach
it to a single cache, chosen across two axes: **side** (scalar vs vector
chain, `--prefetcher-side`) and **level** (L1D vs L2, `--prefetcher-level`),
with prefetching disabled on every other cache. This is the configuration
layer for prefetcher experiments; it is purely Python (no gem5 rebuild).

The four placements are: scalar L1D, scalar L2, vector L1D, vector L2.
`--prefetcher-side` defaults to `vector`, so runs that predate this flag are
unchanged. Selecting a prefetcher still forces `--vector-cache` (the split
hierarchy is what hosts the prefetcher on either chain), so the scalar side
here is the scalar L1D/L2 of the split hierarchy, with vector accesses steered
to their own prefetcher-free chain.

## What gem5 does by default

- A prefetcher is a SimObject parameter on the cache:
  `prefetcher = Param.BasePrefetcher(NULL, "Prefetcher attached to cache")`
  (`src/mem/cache/Cache.py:108`). `NULL` means *no prefetcher*. A cache
  auto-registers whatever prefetcher SimObject it holds when the system is
  instantiated — no manual probe wiring is needed for the queued prefetchers
  used here (only PIF/FDP need `listenFromProbe`).
- The stdlib component caches, however, are **not** NULL: `L1ICache`,
  `L1DCache`, and `L2Cache` all default `PrefetcherCls=StridePrefetcher` and
  run `self.prefetcher = PrefetcherCls()`
  (`src/python/gem5/components/cachehierarchies/classic/caches/l1icache.py:56,67`,
  `.../l1dcache.py:56,67`, `.../l2cache.py:55,67`). So a stock
  `--vector-cache` run previously had a `StridePrefetcher` on **every** cache.
- All prefetcher classes (`StridePrefetcher`, `IndirectMemoryPrefetcher`,
  `IrregularStreamBufferPrefetcher`, `STeMSPrefetcher`) live in
  `src/mem/cache/prefetch/Prefetcher.py`; every behavioral knob is a
  `Param.*` on that class or inherited from `QueuedPrefetcher`/
  `BasePrefetcher` (e.g. `queue_size` L139, `prefetch_on_access` L80).

## New Features

| Component | File |
|---|---|
| `prefetcher_factory.build(name, params)` → zero-arg factory; maps a CLI name (`none`/`stride`/`imp`/`vimp`/`isb`/`stems`) + `--pf-param` dict to `() -> fresh PrefetcherCls(**coerced)`; coerces each value `int → bool → str`; validates each param name against `cls._params`. `needs_mmu(name, params)` reports whether the selection trains on virtual addresses (vimp by default, or explicit `use_virtual_addresses=true`) | `rvv/prefetcher_factory.py` (new) |
| `scalar_l1d_prefetcher` / `scalar_l2_prefetcher` / `vector_l1d_prefetcher` / `vector_l2_prefetcher` ctor kwargs (zero-arg factories); `incorporate_cache` sets `prefetcher = NULL` on **every** cache, then attaches the selected factory to the one chosen node — scalar or vector, L1D or L2 (factory called once per core — a SimObject can't be shared). `prefetcher_needs_mmu=True` additionally calls `registerMMU(cpu.core.mmu)` on the attached prefetcher so page-crossing prefetch targets get translated instead of dropped | `rvv/vector_cache_hierarchy.py` |
| `--prefetcher {none,stride,imp,vimp,isb,stems}`, `--prefetcher-side {scalar,vector}` (default `vector`), `--prefetcher-level {l1,l2}` (default `l2`), repeatable `--pf-param NAME=VALUE`; selecting a prefetcher forces `--vector-cache`; `(side, level)` route the single factory to one of the four `*_prefetcher` kwargs; the MMU is registered automatically when `needs_mmu` says so (shown as `PF MMU:` in the config banner) | `rvv/riscv-rvv-se-ara-prefetcher.py` |

Why a *factory* and not a prefetcher instance: a SimObject instance belongs to
one parent, so each core's cache needs its own. The hierarchy calls the
factory once per core inside its per-core loop.

Why all-NULL-then-attach: it guarantees **at most one active prefetcher** —
the one the CLI selects, on the cache it names — and a clean prefetcher-free
baseline when no `--prefetcher` is given.

### Behavior change (flag in comparisons)

A `--vector-cache` run with **no** `--prefetcher` now has *no* prefetcher on
any cache. Previously it inherited a `StridePrefetcher` on every cache. To
reproduce the old everywhere-stride behavior, the caches would need their
`PrefetcherCls` restored; for these experiments the clean isolation (one
prefetcher under test, on the vector stream only) is the intended default.

## Invocation

```bash
build/RISCV/gem5.opt -d <outdir> rvv/riscv-rvv-se-ara-prefetcher.py \
    --prefetcher imp --prefetcher-level l2 \
    --pf-param max_prefetch_distance=32 --pf-param streaming_distance=8 \
    --vlen 512 --vector-timing-throughput 4 --simd-units 2 \
    <workload-binary> [workload args]
```

`-d <outdir>` is gem5's **output-directory** option and must come **before**
the script name — it is parsed by gem5 itself (`src/python/m5/main.py:101`),
not the config script. (The script reuses `-d` for `--l1d` cache size, a
pre-existing flag, so a `-d` placed *after* the script name sets the scalar
L1D size instead.)

`--prefetcher-level l1` attaches the same prefetcher to the L1D instead of the
L2. `--prefetcher-side scalar` attaches it to the scalar chain instead of the
vector chain — e.g. `--prefetcher-side scalar --prefetcher-level l1` puts it on
the scalar L1D. Omitting `--prefetcher` (or `--prefetcher none`) leaves every
cache prefetcher-free.

```bash
# Same prefetcher, but on the scalar L2 instead of the vector L2:
build/RISCV/gem5.opt -d <outdir> rvv/riscv-rvv-se-ara-prefetcher.py \
    --prefetcher imp --prefetcher-side scalar --prefetcher-level l2 \
    --vlen 512 --vector-timing-throughput 4 --simd-units 2 \
    <workload-binary> [workload args]
```

## `--pf-param` reference (defaults parenthesized)

The factory applies any `Param.*` on the chosen class, so this is a curated
convenience list, not an enforcing allowlist — to expose a new knob just pass
it; to stop tuning one, omit it (its built-in default applies). A typo'd name
fails with a clear `unknown --pf-param … valid: …` error.

- **stride:** `degree(4)`, `distance(0)`, `confidence_threshold(50)`,
  `confidence_counter_bits(3)`, `initial_confidence(4)`.
- **imp** (`IndirectMemoryPrefetcher`): `max_prefetch_distance(16)`,
  `streaming_distance(4)`, `stream_counter_threshold(4)`,
  `prefetch_threshold(2)`, `num_indirect_counter_bits(3)`. Precondition (not
  a knob): IMP `shift_values` must contain the element shift (e.g. 2 for
  4-byte elements) or IMP won't match the gather.
- **vimp** (`VectorIndirectMemoryPrefetcher`, this fork — see
  DOCUMENTATION.MD): `index_size(4)` = bytes per index element in the chunk
  payload (the EEW/8 of the index load, e.g. 4 for `vle32`-loaded `int`
  indices), `index_signed(True)`, `streaming_distance(4)` chunks ahead on the
  index array, `stream_dedup(True)` = emit each stream line once per walk
  (high-water mark; `false` restores the stock re-emit-the-whole-window
  behavior, which inflates `pfLate` with in-cache duplicate hits),
  `stream_counter_threshold(4)`, `prefetch_threshold(2)`,
  `max_indirect_targets(32)` per chunk event, `ipd_indices_per_chunk(8)`,
  `max_indices_per_chunk(64)`, `addr_array_len(4)` (recent-miss FIFO for
  pair matching), `ipd_chunk_history(16)` (chunk index-set history — the
  OoO lag tolerance of the detector), `demotion_chunks(16)` (0 = sticky
  enabled state), `num_indirect_counter_bits(3)`,
  `ipd_train_on_hits(False)`.
  `indirect_delta(0)` = lookahead in chunks for the indirect targets: 0
  issues the current chunk's targets from its own payload; N>0 captures
  index lines at cache-fill time and issues their targets once the demand
  stream is within N chunks (effective lookahead is capped by
  `streaming_distance`; sizing knobs `pending_fill_entries(32)` and
  `pending_index_sets(8)`). Class defaults
  that differ from the other prefetchers: `use_virtual_addresses(True)`
  (requires the MMU, registered automatically), `prefetch_on_access(True)`
  (index payloads are only readable on hits), `queue_size(64)`.
  `shift_values([0,1,2,3,4])` must contain the target element shift (2 for
  4-byte, 3 for 8-byte elements) — it is a `VectorParam`, so it is not
  settable via `--pf-param`.
- **gdp** (`GDPPrefetcher`, this fork — see DOCUMENTATION.MD): the
  Gather Dataflow Prefetcher, built on Tyche's skeleton — records the
  vsext/vsll/vadd ops between a unit-stride producer load and its
  gather and replays them on index lines captured from its own
  stream's fills, so `A[f(B[i])]` (incl. +c biases) prefetches
  exactly; unit-stride producers with no gather attached stream too
  (subsumes a vector stream prefetcher: next chunk = `addr + size`,
  exact on varying partial-vl walks). No confidence/kill/training,
  one-iteration arming. Vector side only (`--prefetcher-side vector`;
  per-core `VectorChainTable` wired automatically).
  `streaming_distance(8)` lines — sets indirect lookahead too; full
  indirect timeliness needs distance x cadence >= 2 memory
  round-trips, so sweep 16-32, `stream_only(False)` ablation (stream
  without capture/replay), `slice_buffer_entries(2)` entries in the
  slice buffer in front of each replay pipeline (watch
  `bufferBusyDrops`; poisson3Db knee at 4), `pipelines(2)`
  concurrently configured producers, `routing_entries(32)` Index
  Routing Table capacity. Table knobs live on the shared
  `VectorChainTable`, not the prefetcher, so they take script flags
  rather than `--pf-param`: `--vector-dct-entries(8)` (min 3) and
  `--vector-max-transform-stages(4)`. One table is built per core, so
  those two flags size the chain table for whichever of
  gdp/vtyche/vtyche2/vhybrid is attached. Class
  defaults as vimp: `use_virtual_addresses(True)` (MMU registered
  automatically), `prefetch_on_access(True)`, `queue_size(64)`.
- **tyche** (`TychePrefetcher`, this fork — see DOCUMENTATION.MD): port
  of the Tyche dependency-chain indirect prefetcher (ChampSim artifact)
  for SCALAR code — it decodes scalar RV64 instructions, so it requires
  the scalar side (`--prefetcher-side scalar`, or `--scalar-prefetcher
  tyche` in dual configs; the per-core `TycheChainTable` is wired
  automatically). Chains root at IP-stride loads; recorded ALU ops
  replay on captured fill values, so any `A[f(B[i])]` the chain ALU
  expresses is covered — the scalar-side sibling of gdp's
  transform-chain replay. `stride_distance(32)` head lookahead iterations,
  `stride_only(False)` = IP-stride prefetches only (the artifact's
  only_stride ablation), `walk_entries(16)` in-flight walk steps
  (AGQ_SIZE), `successors_per_wakeup(4)` (ISQ_WRITE_PORT),
  `max_chain_hops(16)`, `pending_target_entries(32)`. Table knobs are
  on the SimObject, not `--pf-param`: `dct_entries(24)`,
  `ipt_entries(32)`, `dense_threshold(115)`. Class defaults as vimp:
  `use_virtual_addresses(True)` (MMU registered automatically),
  `prefetch_on_access(True)`, `queue_size(64)`.
- **revela** (`RevelaPrefetcher`, this fork — see DOCUMENTATION.MD):
  the ICS'24 Register Vector Length Agnostic prefetcher. Prefetches
  ONLY data the program has announced: the vsetvl AVL (elements the
  strip-mined loop still has to process) marks each unit-stride
  stream's end address, so every emitted line is known-future-accessed
  (near-perfect accuracy, coverage limited to announced streams — the
  paper positions it as a complement to a coverage prefetcher).
  Trigger is a self-clocked every-cycle drain, not cache events.
  Vector side only (`--prefetcher-side vector`; per-core
  `RevelaStreamTable` wired automatically, sized by the script flag
  `--revela-stt-entries(16)`, not `--pf-param`).
  `max_prefetch_distance(64)` lines = the aggressivity ceiling,
  halved at 2/4/8 live streams (the paper's 64/32/16/8 table; its
  sensitivity sweep is 8-128), `degree(1)` lines per min-distance
  stream per evaluation; the trigger evaluates every cycle (the
  paper's fixed cadence, not a param). Class defaults: `use_virtual_addresses
  (True)` (streams are VA-contiguous; MMU registered automatically),
  `prefetch_on_access(True)` (notifies latch the drain's translation
  context), `queue_size(16)` (the paper's prefetch queue). The
  paper's ">=8 free MSHRs" issue gate maps onto the hosting cache's
  `demand_mshr_reserve`, not a prefetcher param.
- **isb** (`IrregularStreamBufferPrefetcher`): `degree(4)`,
  `chunk_size(256)`, `num_counter_bits(2)`.
- **stems** (`STeMSPrefetcher`): `reconstruction_entries(256)`,
  `spatial_region_size(2KiB)`, `add_duplicate_entries_to_rmob(True)`.
- **all (inherited):** `queue_size(32)`, `prefetch_on_access(False)`. Keep
  `on_miss=False`, `use_virtual_addresses=False`, `latency=1` — except for
  `vimp`, whose class defaults override the first two as listed above.

## Verifying correctness across prefetcher choices

Prefetching is microarchitectural, so a kernel must compute the **same
result** with or without any prefetcher. TSVC prints a per-kernel checksum on
stdout; run the kernel once with no prefetcher and once with the prefetcher
under test, then compare the checksum line:

```bash
diff <(grep <kernel> out/none/stdout.txt) <(grep <kernel> out/imp/stdout.txt) \
    && echo MATCH
```

A mismatch means the wiring is altering results (wrong cache attached,
address/translation handling) — a correctness bug to fix before trusting any
timing number, not a legitimate performance result.

## Caveats

1. **At most one prefetcher.** Scalar L1I/L1D/L2 and the unselected vector
   cache are all `NULL`. Confirm engagement in `stats.txt`: with level `l2`,
   `…vector-l2-cache-0.prefetcher.pfIssued > 0`; with level `l1` the
   `pfIssued` stat appears on `…vector-l1d-cache-0.prefetcher` instead. With
   no `--prefetcher`, no cache reports a `prefetcher.pfIssued` stat.
2. **Config-only.** Workload selection and data collection are out of scope;
   this layer just selects and parameterizes the prefetcher.

