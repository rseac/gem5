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

This is the one **core-file** change on this branch. It is low-risk because IMP is the only prefetcher that reads `PrefetchInfo.data` — every other prefetcher ignores it, so their behavior is unchanged.

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