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
classes (which attach a `StridePrefetcher` by default and expose
`PrefetcherCls` — the hook for custom vector-prefetcher experiments).

### 4. CLI (`rvv/riscv-rvv-se-ara-prefetcher.py`)

```bash
build/RISCV/gem5.opt rvv/riscv-rvv-se-ara-prefetcher.py \
    --enable-chaining --vlen 512 --vector-timing-throughput 4 --simd-units 2 \
    --vector-cache --vector-l1d 32KiB --vector-l2 512KiB \
    /path/to/riscv-binary
```

`--vector-cache` selects the split hierarchy; `--vector-l1d`/`--vector-l2`
size the vector chain (scalar sizes stay on `-d`/`-2`). Without the flag
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

