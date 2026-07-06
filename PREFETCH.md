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

Pick and tune a hardware prefetcher (`none`/`stride`/`imp`/`isb`/`stems`) from the command line and attach it to one vector cache via `--prefetcher`, `--prefetcher-level {l1,l2}`, and repeatable `--pf-param NAME=VALUE`. Selecting a prefetcher forces `--vector-cache`; every other cache is left prefetcher-free. See `MEMORY_CONFIG.md` for the mechanism and the per-prefetcher `--pf-param` reference.

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

**Known remaining deviation** (not fixed): gem5 routes each miss to *either* the IPD *or* the prefetch/stream table (the if/else at `indirect_memory.cc:80`), whereas in the paper (Fig. 3) the IPD is a side structure and the stream table sees every access. With the idx2 bound the hijack now lasts at most ~8 misses per episode instead of forever, but the structural serialization remains.

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