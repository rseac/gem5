## Extending gem5's `Request` object

## What a `Request` carries 

A `Request` is created per access by the LSQ (`src/cpu/o3/lsq.cc:1085-1088`)
with: vaddr/paddr, size, byte-enable, `Request::Flags`, requestor ID, **PC**
(`_inst->pcState().instAddr()` — the macroop PC for vector microops), context
ID, plus the dynamic instruction sequence number
(`setReqInstSeqNum`, `src/cpu/o3/lsq.cc:931`) and task ID
(`src/cpu/o3/lsq.cc:799`). Nothing identifies the access as vector, and no
stride/SEW/vl information exists. Every `Packet` carries a shared pointer to
its `Request` (`src/mem/packet.hh:377`), which is how this metadata becomes
visible to caches.

### Step 1 — define the extension (new file, no core edits)

`src/arch/riscv/insts/rvv_stream_ext.hh` (header-only ⇒ no SConscript entry
needed; add a `.cc` + `Source()` only if implementations grow):

```cpp
class RvvStreamExtension : public Extension<Request, RvvStreamExtension>
{
  public:
    enum class Kind { UnitStride, Strided, Indexed, Segmented };

    RvvStreamExtension(Kind kind, int64_t stride_bytes, uint8_t eew_bytes,
                       uint32_t micro_vl, uint32_t micro_idx)
        : kind(kind), strideBytes(stride_bytes), eewBytes(eew_bytes),
          microVl(micro_vl), microIdx(micro_idx) {}

    std::unique_ptr<ExtensionBase>
    clone() const override
    { return std::make_unique<RvvStreamExtension>(*this); }

    const Kind kind;
    const int64_t strideBytes;   // rs2 for strided; eewBytes for unit-stride
    const uint8_t eewBytes;
    const uint32_t microVl;
    const uint32_t microIdx;
};
```

### Step 2 — one attach hook where every O3 data request is born

All single and split-fragment requests are created in exactly one place:
`LSQ::LSQRequest::addReq` (`src/cpu/o3/lsq.cc:1078-1092`). Add a virtual to
`StaticInst` and call it there — this mirrors the hook pattern this fork
already established for `dynamicOpLatency`
(default at `src/cpu/static_inst.hh:396-401`, O3 call site at
`src/cpu/o3/inst_queue.cc:935-942`):

```cpp
// src/cpu/static_inst.hh — default no-op, zero cost for scalar code
virtual void
annotateMemRequest(ExecContext *xc, const RequestPtr &req) const {}

// src/cpu/o3/lsq.cc, inside LSQRequest::addReq, after the Request is built
_inst->staticInst->annotateMemRequest(_inst.get(), req);
```

`_inst` is the `DynInst`, which *is* the O3 `ExecContext`, so the override can
read architectural state. Two core files gain ~4 lines total; the fork's
precedent for this exact shape is `dynamicOpLatency`/`chainingLatency`
(`static_inst.hh`) and `VectorLoadChainEvent` (`lsq_unit.cc`).

### Step 3 — override per microop family (RISC-V side only)

The C++ base classes for the generated microops live in
`src/arch/riscv/insts/vector.hh` (a file this fork already extends):
`VleMicroInst` (`vector.hh:485`), `VseMicroInst` (`vector.hh:510`),
`VlElementMicroInst` (`vector.hh:605`) / `VsElementMicroInst`
(`vector.hh:641`) for strided element accesses, `VlIndexMicroInst`
(`vector.hh:674`) for indexed. One override per family:

```cpp
// VlElementMicroInst (strided loads, e.g. vlse64.v)
void
annotateMemRequest(ExecContext *xc, const RequestPtr &req) const override
{
    int64_t stride = xc->getRegOperand(this, 1);   // Rs2 = source slot 1
    req->setExtension(std::make_shared<RvvStreamExtension>(
        RvvStreamExtension::Kind::Strided, stride,
        width_EEW(machInst.width) / 8, microVl, microIdx));
}
```

Re-reading a source register is side-effect-free. The operand slot is
verifiable ground truth in the generated code: `Vlse64_vMicro::initiateAcc`
reads `Rs1 = xc->getRegOperand(this, 0); Rs2 = xc->getRegOperand(this, 1);`
(`build/RISCV/arch/riscv/generated/exec-ns.cc.inc`, search for
`Vlse64_vMicro::initiateAcc`) — always re-check the slot there if a template
changes. For `VleMicroInst` (unit-stride) pass `Kind::UnitStride` with
`strideBytes = eewBytes`; for indexed, `Kind::Indexed` with stride 0.

### Step 4 — read it in the prefetcher

The prefetch probe hands your prefetcher the packet:
`Base::probeNotify` builds `PrefetchInfo` from `pkt`
(`src/mem/cache/prefetch/base.cc:261-264`), and the listener's entry point
receives a `CacheAccessProbeArg {PacketPtr pkt; CacheAccessor &cache;}`
(`src/mem/cache/cache_probe_arg.hh:79-89`, `src/mem/cache/prefetch/base.hh:84`).
In a custom prefetcher (subclass of `Queued`), read it wherever you still
hold the packet — e.g. an override of `notify`:

```cpp
if (auto ext = pkt->req->getExtension<RvvStreamExtension>()) {
    // ext->kind, ext->strideBytes — train directly, no stride inference
}
```

`getExtension` returns `nullptr` for scalar accesses (the hook default was a
no-op), so scalar traffic trains your baseline path unchanged.

### Step 5 — how it travels through the hierarchy (no changes needed)

| Path | Same extension visible? | Evidence |
|---|---|---|
| LSQ → L1 (demand access, hit or miss) | yes — packet carries the original `RequestPtr` | `src/mem/packet.hh:377` |
| L1 miss → L2/L3 demand fetch | **yes** — the downstream miss packet reuses the same request: `new Packet(cpu_pkt->req, cmd, blkSize)` | `src/mem/cache/cache.cc:553` |
| `Request` copies (e.g. checker) | yes — deep-cloned | `src/mem/request.hh:515-516`, `src/base/extensible.hh:123-130` |
| Writebacks / evictions | no — caches mint fresh `Request`s | `src/mem/cache/base.cc:1765,1808` |
| Prefetcher-generated prefetches | no — new `Request` (copies flags/PC only); your prefetcher may re-attach the extension if you want it visible at lower levels | `src/mem/cache/prefetch/queued.cc:380-383` |

Caveat: under MSHR coalescing, the packet forwarded downstream belongs to the
*first* miss to that line, so an L2-attached consumer sees one extension per
line fill, not per coalesced access. L1 prefetcher training is unaffected —
`probeNotify` fires per access, before MSHR merging.

Nothing in crossbars, caches, or memory controllers needs modification; they
never interpret extensions. Squash safety is automatic: the extension is a
`shared_ptr` member of the request, freed when the last packet/MSHR reference
drops.

# RVVExtension Request Extension 

Tags every O3 data-memory `Request` with the **OpClass of the instruction
that created it**, plus — for strided vector accesses — the **rs2 register
value** (the architectural byte stride), and makes `Packet::print()` show
both. Every cache debug line then reveals what kind of access it is:

```
1353000: board.cache_hierarchy.l1d-cache-0: access for ReadReq [2868:286f] type=SimdStridedLoad rs2=16 miss
```

## What gem5 does by default

- `Request` carries no information about the instruction that produced it
  beyond the PC (see "What a `Request` carries today" above). Caches and
  prefetchers cannot tell a `vlse64.v` element access from a scalar `ld`,
  and a prefetcher must *infer* the stride the instruction already knows.
- `Packet::print()` (`src/mem/packet.cc:368`) prints only the command name,
  the inclusive physical byte range `[start:end]`, and request flags
  (`(s)`/`IF`/`UC`/`ES`/`PoC`/`PoU`). Every cache `DPRINTF` that embeds
  `pkt->print()` inherits this format.
- gem5 already classifies RVV memory accesses at decode: the decoder assigns
  per-kind op classes (`SimdUnitStrideLoadOp`, `SimdStridedLoadOp`,
  `SimdIndexedLoadOp`, …) as instruction flags
  (`src/arch/riscv/isa/decoder.isa:607,755`), retrievable from any
  instruction via `StaticInst::opClass()`. The stride for `vlse*`/`vsse*`
  is the rs2 source register, re-read freely via
  `ExecContext::getRegOperand` — the generated microop code reads
  `Rs2 = xc->getRegOperand(this, 1)` for both loads and stores
  (`build/RISCV/arch/riscv/generated/exec-ns.cc.inc`, `Vlse64_vMicro::initiateAcc`
  and `Vsse64_vMicro::initiateAcc`). None of this traveled with the request
  until now.

## New Features

Mechanism #3 from "Extending `Request` with Custom Instruction Metadata"
above (the `Extensible<Request>` framework, `src/mem/request.hh:97`,
`src/base/extensible.hh`), combined with the Step-2 attach-hook pattern
(mirroring this fork's `dynamicOpLatency` precedent in `static_inst.hh`).

| Change | File |
|---|---|
| **New** `RVVExtension` — header-only extension class holding an `OpClass` plus an `int64_t rs2` (default 0); `getInstType()`, `getRs2()`, `toString()` | `src/mem/rvv_ext.hh` |
| **New virtual** `StaticInst::annotateMemRequest(ExecContext*, const RequestPtr&)` — the per-instruction tagging hook | decl `src/cpu/static_inst.hh:424`, default impl `src/cpu/static_inst.cc:98` (attaches `RVVExtension(opClass())` — every memory instruction, scalar included) |
| Strided-microop overrides: when `has_rs2` is set, attach `RVVExtension(opClass(), rs2)` with rs2 read from source slot 1; otherwise fall back to the default | `src/arch/riscv/insts/vector.hh:630` (`VlElementMicroInst`, covers `vlse*`/`vlsseg*`) and `:680` (`VsElementMicroInst`, covers `vsse*`/`vssseg*`) |
| Hook call sites: every single/split-fragment request the LSQ creates, plus the split-access main request | `src/cpu/o3/lsq.cc:1117` (in `LSQRequest::addReq`) and `:969` (in `SplitDataRequest::initiateTranslation`) |
| `Packet::print()` appends ` type=<OpClass>` when the extension is present, and ` rs2=<stride>` (signed decimal) only when the type is `SimdStridedLoad` or `SimdStridedStore` | `src/mem/packet.cc:372-384` |

No SConscript change: the extension is header-only. The operand-slot
knowledge (rs2 = source slot 1) lives only in the RISC-V microop classes
that own it, guarded by their `has_rs2` flag — the same classes whose
generated `initiateAcc` reads that slot, so the two cannot silently
diverge in meaning. Reading a source register at request-creation time is
side-effect-free and safe: `addReq` runs inside `initiateAcc`, when the
microop's sources are ready by definition.

## Coverage and caveats

- **Tagged with type**: all data requests born in `LSQ::LSQRequest::addReq`
  (`src/cpu/o3/lsq.cc:1084`) — the single creation point for O3
  loads/stores/AMOs. Demand misses forwarded to L2/L3 keep the tag because
  the downstream miss packet reuses the same request
  (`src/mem/cache/cache.cc:553`); request copies deep-clone the extension
  (`src/mem/request.hh:515`, `src/base/extensible.hh:123`).
- **Tagged with rs2**: only microops of `VlElementMicroInst` /
  `VsElementMicroInst` with `has_rs2 == true` — i.e. `vlse*`, `vlsseg*`,
  `vsse*`, `vssseg*`. Variants of those classes without an rs2 operand
  fall back to type-only tagging.
- **rs2 printed**: only when the OpClass is `SimdStridedLoad` or
  `SimdStridedStore`. The field defaults to 0 in all other extensions and
  is not shown.
- **Untagged** (print shows no `type=`): instruction fetches, cache
  writebacks/evictions (`src/mem/cache/base.cc:1765,1808`),
  prefetcher-generated requests (`src/mem/cache/prefetch/queued.cc:380`),
  page-table walks, anything from non-O3 CPU models, and the tick-0
  functional loader writes.
- Under MSHR coalescing the packet sent downstream belongs to the *first*
  miss to that line, so L2 sees one tag per line fill, not one per
  coalesced access.

## How to read it elsewhere

Anywhere a packet is in hand (e.g. a custom prefetcher's `notify`):

```cpp
#include "mem/rvv_ext.hh"

if (auto ext = pkt->req->getExtension<RVVExtension>()) {
    if (ext->getInstType() == enums::SimdStridedLoad) {
        int64_t stride = ext->getRs2();   // train directly, no inference
    }
}
```

`getExtension` returns `nullptr` for untagged requests, so scalar/legacy
paths are unaffected.