# Load/Store gem5 Datapath

This section traces a memory instruction through the O3 CPU from **issue** to
**commit**, citing the exact code that implements each step. It covers the
stock gem5 machinery first and flags where this fork's ARA additions hook in
(`dynamicOpLatency`, chaining, `VectorLoadChainEvent`). Both scalar loads/stores
and RVV vector memory microops follow the same path — the differences are in
the ISA layer (§4) and the fork's vector hooks (§2.2, §7.1).

## Overview

gem5 splits every timing-mode memory instruction into two halves, defined as
virtual functions on the static instruction (`src/cpu/static_inst.hh:505-516`):

- **`initiateAcc()`** — computes the effective address and *starts* the access
  (translation + cache request). Called during execute.
- **`completeAcc(pkt)`** — consumes the returned data packet and writes the
  destination register. Called when the cache responds.

The pipeline stages between issue and commit:

```
IQ (scheduleReadyInsts)            issue, FU + latency selection
  └─> IEW::executeInsts            calls LSQ::executeLoad / executeStore
        └─> DynInst::initiateAcc   ISA code computes EA, calls initiateMemRead/writeMem
              └─> LSQ::pushRequest LSQRequest created, TLB translation
                    └─> LSQUnit::read / write
                          ├─ store-to-load forwarding, or
                          └─ buildPackets → sendPacketToCache → D-cache
                                └─> DcachePort::recvTimingResp
                                      └─> LSQUnit::completeDataAccess
                                            └─> writeback → completeAcc
                                                  └─> IEW::instToCommit        [vector loads: +1 cycle, fork]
                                                        └─> writebackInsts     wakes dependents
                                                              └─> Commit       setCanCommit → retireHead
                                                                    └─> doneSeqNum → LSQ commitStores
                                                                          └─> writebackStores → cache (stores only)
```

Loads get their data **before** commit; stores commit first and write to the
cache **after** commit, from the store queue.

## File map

| File | Role in the datapath |
|---|---|
| `src/cpu/o3/inst_queue.cc/.hh` | Issue scheduling, FU/latency selection, replay lists; fork's `dynamicOpLatency`/chaining hooks |
| `src/cpu/o3/mem_dep_unit.cc/.hh` | Memory dependence tracking; gates mem-inst readiness |
| `src/cpu/o3/store_set.cc/.hh` | Store-set predictor used by the MemDepUnit |
| `src/cpu/o3/fu_pool.cc/.hh` | Functional-unit allocation and static op latencies |
| `src/cpu/o3/FuncUnitConfig.py` | Upstream FU definitions (`RdWrPort` serves all memory op classes) |
| `src/cpu/o3/AraConfig.py` | Fork: ARA FU pool (`AraSIMD_Pipelined` covers vector ld/st address generation) |
| `src/cpu/o3/iew.cc/.hh` | Execute + writeback stage; dispatches to the LSQ, wakes dependents |
| `src/cpu/o3/lsq.cc/.hh` | LSQ wrapper: `pushRequest`, `LSQRequest` lifecycle, `DcachePort` |
| `src/cpu/o3/lsq_unit.cc/.hh` | Per-thread LQ/SQ: execute, forwarding, writeback, store commit; fork's `VectorLoadChainEvent` |
| `src/cpu/o3/dyn_inst.cc/.hh` | Dynamic instruction: `initiateAcc`/`completeAcc` dispatch, `initiateMemRead`/`writeMem` |
| `src/cpu/o3/cpu.hh` | `pushRequest` wrapper forwarding to the LSQ |
| `src/cpu/translation.hh` | `DataTranslation` glue between MMU and LSQRequest |
| `src/arch/riscv/tlb.cc` | `translateTiming`: the actual address translation |
| `src/cpu/o3/commit.cc`, `src/cpu/o3/rob.cc` | Commit stage and ROB retirement |
| `src/cpu/o3/comm.hh` | Inter-stage time-buffer structs (`doneSeqNum`, squash signals) |
| `src/cpu/static_inst.hh` | `initiateAcc`/`completeAcc` virtuals; fork's `dynamicOpLatency`/`chainingLatency` |
| `src/arch/riscv/isa/formats/mem.isa` | Scalar load/store `initiateAcc`/`completeAcc` templates |
| `src/arch/riscv/isa/templates/vector_mem.isa`, `src/arch/riscv/isa/formats/vector_mem.isa` | RVV memory macroop/microop templates |
| `src/arch/riscv/insts/vector.hh` | Vector inst base classes; fork's latency/chaining formulas |

## 1. Before issue: how a memory instruction enters the machinery

Two things happen at dispatch that the issue stage later depends on:

**IQ insertion registers the instruction with the MemDepUnit.** Unlike normal
instructions, memory references are not added directly to the ready list
(`src/cpu/o3/inst_queue.cc:686-690`):

```cpp
if (new_inst->isMemRef()) {
    memDepUnit[new_inst->threadNumber].insert(new_inst);
} else {
    addIfReady(new_inst);
}
```

`MemDepUnit::insert` (`src/cpu/o3/mem_dep_unit.cc:192`) consults the store-set
predictor for a store this instruction probably depends on
(`src/cpu/o3/mem_dep_unit.cc:225-227`):

```cpp
InstSeqNum dep = depPred.checkInst(inst->pcState().instAddr());
if (dep != 0)
    producing_stores.push_back(dep);
```

If a producing store is in flight, the instruction's entry carries a non-zero
`memDeps` count and cannot issue until that store executes.

**The LSQ allocates a queue entry.** `LSQUnit::insertLoad`
(`src/cpu/o3/lsq_unit.cc:358`) and `insertStore`
(`src/cpu/o3/lsq_unit.cc:421`) reserve the LQ/SQ slots and record the
iterators (`lqIt`/`sqIt`) later used for forwarding and violation checks.

## 2. Issue: `InstructionQueue::scheduleReadyInsts`

### 2.1 Becoming ready

When all source registers are ready, `addIfReady` does **not** put a memory
instruction on the ready list itself — it defers to the MemDepUnit
(`src/cpu/o3/inst_queue.cc:1576-1591`). `MemDepUnit::regsReady` only releases
the instruction once its memory dependences are also resolved
(`src/cpu/o3/mem_dep_unit.cc:347-366`):

```cpp
inst_entry->regsReady = true;
if (inst_entry->memDeps == 0) {
    moveToReady(inst_entry);
}
```

`moveToReady` calls back into the IQ via `iqPtr->addReadyMemInst`
(`src/cpu/o3/mem_dep_unit.cc:604-612`, `src/cpu/o3/inst_queue.cc:1218`),
placing the instruction on the per-opclass ready queue like any other.

### 2.2 Selecting an FU and a latency

`scheduleReadyInsts` (`src/cpu/o3/inst_queue.cc:855`) first drains the
deferred/blocked replay lists back onto the ready list
(`src/cpu/o3/inst_queue.cc:863-870`), then walks the ready queues. For each
instruction it acquires a functional unit from the pool —
`FUPool::getUnit` scans for a non-busy unit with the right capability
(`src/cpu/o3/fu_pool.cc:164`). Scalar loads/stores use the `MemRead`/`MemWrite`
op classes; vector memory microops use `SimdUnitStrideLoad`,
`SimdStridedLoad`, etc. Upstream, all of these are served by `RdWrPort`
(`src/cpu/o3/FuncUnitConfig.py:187-210`); in this fork's ARA configuration the
vector ld/st *address-generation* op classes live in `AraSIMD_Pipelined` with
`opLat=1` (`src/cpu/o3/AraConfig.py:33`, opList at `AraConfig.py:85-95`),
because the real memory latency is modeled by the cache hierarchy, not the FU.

**Fork addition — dynamic latency.** Upstream gem5 takes the issue latency
from the FU description (`fu_pool->getOpLatency`). This fork first asks the
static instruction itself (`src/cpu/o3/inst_queue.cc:935-942`):

```cpp
if (auto dyn_lat = issuing_inst->staticInst->dynamicOpLatency(
        issuing_inst->tcBase());
    dyn_lat > Cycles(0)) {
    op_latency = dyn_lat;
} else {
    op_latency = fu_pool->getOpLatency(op_class);
}
```

`dynamicOpLatency` is a fork-added virtual on `StaticInst` defaulting to 0
(`src/cpu/static_inst.hh:396-401`); `VectorMicroInst` overrides it to compute
ARA occupancy from `microVl`, `sew`, and the lane count
(`src/arch/riscv/insts/vector.hh:202`).

**Fork addition — compute chaining (does NOT apply to memory ops).** For
instructions whose `chainingLatency` is shorter than their `op_latency`, the
fork schedules an early `WakeDependents` event — but explicitly **excludes
memory references** to avoid double-issue in the LSQ
(`src/cpu/o3/inst_queue.cc:972-996`):

```cpp
if (chaining_latency < op_latency && !issuing_inst->isMemRef()) {
    auto wakeup = new WakeDependents(issuing_inst, this);
    cpu->schedule(wakeup, cpu->clockEdge(Cycles(chaining_latency - 1)));
}
```

Vector *loads* chain through a different mechanism in the LSQ instead (§7.1).
`WakeDependents::process` pushes the instruction onto `instsToExecute` early
(`src/cpu/o3/inst_queue.cc:201-216`).

### 2.3 Issue-to-execute delay

Issued instructions land on `instsToExecute` and a slot count travels through
the `issueToExecQueue` time buffer; IEW reads it `issueToExecuteDelay` cycles
later (`src/cpu/o3/iew.cc:84`, wire setup at `src/cpu/o3/iew.cc:111`):

```cpp
fromIssue = issueToExecQueue.getWire(-issueToExecuteDelay);
```

This is the "1 (issue)" term in this fork's dependency-delay formula
(`total = 1 + latencyModel->getLatency(...)`, see `README_ARA_sim.md`).

## 3. Execute: `IEW::executeInsts` hands off to the LSQ

`executeInsts` (`src/cpu/o3/iew.cc:1138`) pops each instruction
(`src/cpu/o3/iew.cc:1152-1159`) and branches on type. Loads
(`src/cpu/o3/iew.cc:1210-1216`):

```cpp
} else if (inst->isLoad()) {
    // Loads will mark themselves as executed, and their writeback
    // event adds the instruction to the queue to commit
    fault = ldstQueue.executeLoad(inst);
```

Stores are symmetric (`src/cpu/o3/iew.cc:1228-1233`). In both cases, if
translation was delayed by a page-table walk the instruction is parked via
`instQueue.deferMemInst` (`src/cpu/o3/inst_queue.cc:1262`) and replayed later.
`LSQ::executeLoad`/`executeStore` simply dispatch to the right thread's
`LSQUnit` (`src/cpu/o3/lsq.cc:284-289`, `src/cpu/o3/lsq.cc:292`).

`LSQUnit::executeLoad` (`src/cpu/o3/lsq_unit.cc:635`) runs the first half of
the access (`src/cpu/o3/lsq_unit.cc:650`):

```cpp
load_fault = inst->initiateAcc();
```

then checks for memory-order violations against younger loads
(`src/cpu/o3/lsq_unit.cc:694-700`). `executeStore`
(`src/cpu/o3/lsq_unit.cc:707`) does the same with `initiateAcc` at
`src/cpu/o3/lsq_unit.cc:723` and a violation sweep over younger loads at
`src/cpu/o3/lsq_unit.cc:764`. Note what `executeStore` does *not* do: it does
not send anything to memory. It only computes the address and stages the data
in the SQ; regular stores become eligible for cache writeback only after
commit (§10). (Store-conditionals/atomics set `canWB` immediately,
`src/cpu/o3/lsq_unit.cc:756-762`.)

`DynInst::initiateAcc` (`src/cpu/o3/dyn_inst.cc:365-378`) forwards to the
ISA-generated `staticInst->initiateAcc(this, traceData)`.

## 4. The ISA layer: what `initiateAcc`/`completeAcc` actually are

### 4.1 Scalar loads/stores

RISC-V scalar memory instructions are generated from templates in
`src/arch/riscv/isa/formats/mem.isa`. The decoder supplies only the
access-specific snippet, e.g. `lw` (`src/arch/riscv/isa/decoder.isa:546`):
`Rd_sd = Mem_sw;`. The templates wrap it:

- `LoadInitiateAcc` (`src/arch/riscv/isa/formats/mem.isa:149-162`) — computes
  `EA` and calls `initiateMemRead(xc, traceData, EA, Mem, memAccessFlags)`.
- `LoadCompleteAcc` (`mem.isa:164-179`) — extracts data from the returned
  packet (`getMemLE(pkt, Mem, traceData)`) and writes the destination register
  (`%(op_wb)s`).
- `StoreInitiateAcc` (`mem.isa:209-233`) — computes `EA`, materializes the
  store data, and calls `writeMemTimingLE(...)`.
- `StoreCompleteAcc` (`mem.isa:235-242`) — empty; returns `NoFault`.

`initiateMemRead`/`writeMem` land on the DynInst, which pushes the access into
the CPU (`src/cpu/o3/dyn_inst.cc:411-418` and `432-440`):

```cpp
return cpu->pushRequest(..., /* ld */ true, nullptr, size, addr, flags, ...);
```

`CPU::pushRequest` is a thin wrapper around
`iew.ldstQueue.pushRequest` (`src/cpu/o3/cpu.hh:580-588`).

### 4.2 Vector memory instructions

RVV loads/stores are **macroops split into microops** at decode. The
unit-stride constructor (`VleConstructor`,
`src/arch/riscv/isa/templates/vector_mem.isa:58-80`) creates one microop per
VLEN-worth of data:

```cpp
const int32_t micro_vlmax = vlen / width_EEW(_machInst.width);
const uint32_t num_microops = ceil((float) this->vl / (micro_vlmax));
...
microop = new %(class_name)sMicro(_machInst, micro_vl, i, elen, vlen);
microop->setDelayedCommit();
```

Each microop is an ordinary load to the LSQ. `VleMicroInitiateAcc`
(`vector_mem.isa:193-223`) computes the EA, sizes the access as
`width_EEW(machInst.width) / 8 * microVl` bytes, and calls
`initiateMemRead(xc, EA, mem_size, memAccessFlags, byte_enable)`.
`VleMicroCompleteAcc` (`vector_mem.isa:225`) memcpys the packet into the
vector register and applies mask/tail policy. Stores gather elements into a
buffer with a per-element `byte_enable` mask and call `xc->writeMem`
(`VseMicroInitiateAcc`, `vector_mem.isa:389`).

**Strided** loads (`vlse64.v`) use element-granular microops: the
`VlStrideOp` format defines `EA = Rs1 + Rs2 * ei + offset`
(`src/arch/riscv/isa/formats/vector_mem.isa:206-218`), so each element is its
own LSQ request — this is why a strided vector load appears in `ExecMicro`
traces (and to the prefetcher) as a sequence of small accesses with stride
`Rs2`. Segmented loads follow the same pattern
(`VlSegMicroInitiateAcc`, `vector_mem.isa:1549`).

## 5. Translation: the `LSQRequest` lifecycle

`LSQ::pushRequest` (`src/cpu/o3/lsq.cc:756`) wraps the access in an
`LSQRequest` — `SplitDataRequest` if it crosses a cache line, otherwise
`SingleDataRequest` (`src/cpu/o3/lsq.cc:781-806`) — and starts translation:

```cpp
} else if (needs_burst) {
    request = new SplitDataRequest(&thread[tid], inst, isLoad, addr,
            size, flags, data, res);
} else {
    request = new SingleDataRequest(&thread[tid], inst, isLoad, addr,
            size, flags, data, res, std::move(amo_op));
}
...
request->initiateTranslation();
```

`SingleDataRequest::initiateTranslation` (`src/cpu/o3/lsq.cc:924-942`) marks
`translationStarted`, saves the request on the instruction
(`_inst->savedRequest = this`), and sends the fragment to the MMU
(`src/cpu/o3/lsq.cc:1130-1135`):

```cpp
_port.getMMUPtr()->translateTiming(req(i), _inst->thread->getTC(),
        this, isLoad() ? BaseMMU::Read : BaseMMU::Write);
```

`LSQRequest` *is* the `BaseMMU::Translation` callback object. The RISC-V TLB
translates synchronously when it can (`src/arch/riscv/tlb.cc:622-632`):

```cpp
Fault fault = translate(req, tc, translation, mode, delayed);
if (!delayed)
    translation->finish(fault, req, tc, mode);
else
    translation->markDelayed();
```

A delayed walk is what triggers the `isTranslationDelayed()` deferral in §3.
On completion, `SingleDataRequest::finish` (`src/cpu/o3/lsq.cc:844-873`)
records the physical address on the instruction and flips the state to
`Request` (or `Fault`), and sets `translationCompleted(true)`.

Back in `pushRequest`, once translation is complete the effective address is
recorded and the access proper is performed (`src/cpu/o3/lsq.cc:809-822`):

```cpp
inst->effAddr = request->getVaddr();
...
if (isLoad)
    fault = read(request, inst->lqIdx);
else
    fault = write(request, data, inst->sqIdx);
```

## 6. Loads: forwarding or cache access (`LSQUnit::read`)

`LSQUnit::read` (`src/cpu/o3/lsq_unit.cc:1385`) first searches the store
queue backward from the load's `sqIt` for an older store covering the same
bytes (`src/cpu/o3/lsq_unit.cc:1453-1457`). Three outcomes:

1. **Full forwarding** — the store's SQ data covers the load
   (`AddrRangeCoverage::FullAddrRangeCoverage`,
   `src/cpu/o3/lsq_unit.cc:1519`): the data is copied, a response packet is
   fabricated, and a `WritebackEvent` is scheduled **this tick**
   (`src/cpu/o3/lsq_unit.cc:1585-1591`) — the load never touches the cache
   (`stats.forwLoads`).
2. **Partial coverage** — the load stalls until the store leaves the SQ; it is
   rescheduled through the IQ replay machinery.
3. **No match** — real memory access (`src/cpu/o3/lsq_unit.cc:1666-1674`):

```cpp
request->buildPackets();
request->sendPacketToCache();
if (!request->isSent()) {
    if (!lsq->cacheBlocked()) {
        iewStage->retryMemInst(load_inst);
   } else {
        iewStage->blockMemInst(load_inst);
   }
}
```

`sendPacketToCache` goes through `LSQUnit::trySendPacket` to the CPU's
`DcachePort`. If the cache refuses (no MSHRs, blocked port), the load is
parked on the IQ's blocked list (`src/cpu/o3/inst_queue.cc:1268`) and replayed
when the cache unblocks (`LSQ::recvReqRetry`, `src/cpu/o3/lsq.cc:421`).

Stores at this point only deposit data: `LSQUnit::write`
(`src/cpu/o3/lsq_unit.cc:1680`) memcpys the data into the SQ entry and
returns.

## 7. Cache response: `completeDataAccess` → `writeback` → `completeAcc`

The response packet arrives at `LSQ::DcachePort::recvTimingResp`
(`src/cpu/o3/lsq.cc:1480-1492`), which recovers the originating request from
the packet's sender state (`src/cpu/o3/lsq.cc:446-455`):

```cpp
LSQRequest *request = dynamic_cast<LSQRequest*>(pkt->senderState);
```

`SingleDataRequest::recvTimingResp` (`src/cpu/o3/lsq.cc:1168-1176`) calls
`LSQUnit::completeDataAccess` (`src/cpu/o3/lsq_unit.cc:133`). For loads (and
SC/atomics) this runs the second half of the access
(`src/cpu/o3/lsq_unit.cc:194-211`), i.e. `writeback(inst, pkt)`.

`LSQUnit::writeback` (`src/cpu/o3/lsq_unit.cc:1129`) executes
(`src/cpu/o3/lsq_unit.cc:1140-1145`):

```cpp
inst->setExecuted();
inst->completeAcc(pkt);   // ISA template writes the dest register
```

and then hands the instruction to IEW's writeback queue.

### 7.1 Fork addition: `VectorLoadChainEvent`

Upstream gem5 calls `iewStage->instToCommit(inst)` immediately after
`completeAcc`. This fork inserts a 1-cycle event for vector loads when
chaining is enabled (`src/cpu/o3/lsq_unit.cc:1181-1203`):

```cpp
if (inst->isVector() && inst->isLoad() && cpu->enableVectorChaining) {
    auto *ev = new VectorLoadChainEvent(inst, this);
    cpu->schedule(ev, cpu->clockEdge(Cycles(1)));
    iewStage->activityThisCycle();
} else {
    iewStage->instToCommit(inst);
    ...
}
```

The event (class at `src/cpu/o3/lsq_unit.hh:463-473`, `process()` at
`src/cpu/o3/lsq_unit.cc:96-111`) wakes the CPU and performs the deferred
`instToCommit`. Combined with the implicit 1-cycle
`instToCommit → writebackInsts` stage, this models ARA's
`CHAINING_OVERHEAD = 2` (VRF write + operand-request handshake), and because
it fires after the *real* cache response, the chain delay is measured from
actual data availability — a cache miss naturally pushes dependent wakeup out.
This is why vector loads are excluded from the issue-time `WakeDependents`
chaining path (§2.2): their first-element-ready time depends on the memory
system, not on a fixed FU pipeline depth.

## 8. IEW writeback: waking dependents

`IEW::instToCommit` (`src/cpu/o3/iew.cc:593`) places the instruction in a free
slot of the `iewQueue` writeback buffer. The same cycle's `writebackInsts`
(`src/cpu/o3/iew.cc:1380`) then wakes consumers and updates the scoreboard
(`src/cpu/o3/iew.cc:1403-1418`):

```cpp
if (!inst->isSquashed() && inst->isExecuted() && inst->getFault() == NoFault) {
    int dependents = instQueue.wakeDependents(inst);
    for (int i = 0; i < inst->numDestRegs(); i++) {
        ...
        scoreboard->setReg(inst->renamedDestIdx(i));
    }
}
```

This is the moment a dependent vector compute op waiting on a `vle64.v`
becomes ready in the IQ.

## 9. Commit

Commit reads the writeback queue `iewToCommitDelay` cycles later
(`src/cpu/o3/commit.cc:262`) and marks instructions
(`Commit::markCompletedInsts`, `src/cpu/o3/commit.cc:1328`,
`setCanCommit()` at `commit.cc:1342`).

`commitInsts` (`src/cpu/o3/commit.cc:899`) retires up to `commitWidth`
instructions per cycle (`commit.cc:918`), but only when the ROB head is ready
(`commit.cc:941-944`; `ROB::isHeadReady` checks `readyToCommit()` on the list
head, `src/cpu/o3/rob.cc:264-269`).

`commitHead` (`src/cpu/o3/commit.cc:1111`) contains the load/store asymmetry
(`src/cpu/o3/commit.cc:1180-1183`):

```cpp
// Stores mark themselves as completed.
if (!head_inst->isStore() && inst_fault == NoFault) {
    head_inst->setCompleted();
}
```

A load is fully done at commit; a **store is allowed to retire from the ROB
while its data has not yet been written to memory** — it will mark itself
completed in `storePostSend` (§10). The head entry is then retired
(`rob->retireHead(tid)`, `src/cpu/o3/commit.cc:1273-1274`,
`src/cpu/o3/rob.cc:236-252`) and the committed sequence number is broadcast
back (`src/cpu/o3/commit.cc:1012-1013`, struct field
`src/cpu/o3/comm.hh:202`):

```cpp
toIEW->commitInfo[tid].doneSeqNum = head_inst->seqNum;
```

IEW picks this up next cycle and advances the LSQ
(`src/cpu/o3/iew.cc:1490-1499`):

```cpp
ldstQueue.commitStores(fromCommit->commitInfo[tid].doneSeqNum,tid);
ldstQueue.commitLoads(fromCommit->commitInfo[tid].doneSeqNum,tid);
```

`commitLoads` (`src/cpu/o3/lsq_unit.cc:794`) simply frees LQ entries up to
that sequence number.

## 10. Stores after commit: SQ → cache

`LSQUnit::commitStores` (`src/cpu/o3/lsq_unit.cc:805-828`) walks the SQ in age
order and marks every committed entry writable (`x.canWB() = true`).
Each cycle, `writebackStores` (`src/cpu/o3/lsq_unit.cc:841`) drains writable
stores to the cache: it copies the SQ data into the instruction, builds and
sends the packet (`src/cpu/o3/lsq_unit.cc:893-907`):

```cpp
storeWBIt->committed() = true;
...
memcpy(inst->memData, storeWBIt->data(), request->_size);
request->buildPackets();
```

After a successful send, `storePostSend` (`src/cpu/o3/lsq_unit.cc:1098-1126`)
marks the store instruction completed (for non-SC stores) and unstalls any
load that was blocked on partial forwarding. When the cache response returns,
`completeDataAccess` routes plain stores to `completeStore`
(`src/cpu/o3/lsq_unit.cc:212-216`, function at `lsq_unit.cc:1207`), which
frees the SQ entry. If the cache rejects the packet, `isStoreBlocked` is set
and `recvRetry` re-sends on the next port retry
(`src/cpu/o3/lsq_unit.cc:1342`).

## 11. Memory-order violations and squash

`checkViolations` (`src/cpu/o3/lsq_unit.cc:555`) compares the just-translated
address against younger loads that already executed. On overlap it records a
`memDepViolator` and returns a fault to IEW, which squashes from the violator
and trains the predictor (`src/cpu/o3/iew.cc:1403` region; squash setup in
`squashDueToMemOrder`, `src/cpu/o3/iew.cc:507-519`):

```cpp
toCommit->squash[tid] = true;
toCommit->squashedSeqNum[tid] = inst->seqNum;
...
toCommit->includeSquashInst[tid] = true;
```

The violation is also fed back into the store-set predictor via
`InstructionQueue::violation → MemDepUnit::violation → depPred.violation`, so
the next dynamic instance of that load waits for the conflicting store.

## 12. Summary of fork additions on this path

| Hook | Upstream behavior | Fork behavior | Where |
|---|---|---|---|
| `dynamicOpLatency` | FU latency fixed per op class (`fu_pool->getOpLatency`) | Static inst can override per-instance latency (VLEN/SEW/lane-dependent) | `src/cpu/static_inst.hh:396-401`, `src/cpu/o3/inst_queue.cc:935-942`, `src/arch/riscv/insts/vector.hh:202` |
| `chainingLatency` + `WakeDependents` | Dependents wake at FU completion | Dependents of vector *compute* ops wake when first elements are ready; memory ops excluded (`!isMemRef()`) | `src/cpu/static_inst.hh:408-412`, `src/cpu/o3/inst_queue.cc:972-996`, `src/arch/riscv/insts/vector.hh:281-300` |
| `VectorLoadChainEvent` | Loads call `instToCommit` immediately after `completeAcc` | Vector loads defer 1 cycle to model ARA's VRF-write + handshake overhead, measured from real cache-response time | `src/cpu/o3/lsq_unit.hh:463-473`, `src/cpu/o3/lsq_unit.cc:96-111`, `src/cpu/o3/lsq_unit.cc:1181-1203` |
| `AraSIMD_Pipelined` FU pool | All mem op classes on `RdWrPort` | Vector ld/st address generation on a 2-slot pipelined ARA FU (`opLat=1`); memory time comes from the cache model | `src/cpu/o3/FuncUnitConfig.py:187-210`, `src/cpu/o3/AraConfig.py:33` |

For prefetcher work, the observation point that matters is §6: every vector
memory microop becomes an ordinary `LSQRequest` → D-cache packet, so cache-side
prefetchers (`src/mem/cache/prefetch/`) see unit-stride RVV loads as
VLEN-sized bursts and strided RVV loads as per-element accesses with the
architectural stride.

# Extending `Request` with Custom Instruction Metadata

Goal: carry RVV instruction context — access kind (unit-stride / strided /
indexed / segmented), the constant stride value in `rs2`, element width — from
the CPU down to cache-side components (prefetchers), so a custom prefetcher
does not have to *infer* what the instruction already knows.

## What a `Request` carries today

A `Request` is created per access by the LSQ (`src/cpu/o3/lsq.cc:1085-1088`)
with: vaddr/paddr, size, byte-enable, `Request::Flags`, requestor ID, **PC**
(`_inst->pcState().instAddr()` — the macroop PC for vector microops), context
ID, plus the dynamic instruction sequence number
(`setReqInstSeqNum`, `src/cpu/o3/lsq.cc:931`) and task ID
(`src/cpu/o3/lsq.cc:799`). Nothing identifies the access as vector, and no
stride/SEW/vl information exists. Every `Packet` carries a shared pointer to
its `Request` (`src/mem/packet.hh:377`), which is how this metadata becomes
visible to caches.

## The three extension mechanisms already in this codebase

### 1. Architecture-specific flag bits (cheap, tiny)

`Request::Flags` reserves its low 8 bits for the ISA:
`ArchFlagsType` (`src/mem/request.hh:101`), mask `ARCH_BITS = 0x000000FF`
(`src/mem/request.hh:114`), accessor `getArchFlags()`
(`src/mem/request.hh:883-887`).

**Existing RISC-V use**: `src/arch/riscv/memflags.hh` defines `XlateFlags`
inside these 8 bits (`HLVX = 1ULL << 3`, `FORCE_VIRT`, …) and notes the lower
3 bits are taken by MMU alignment encoding. They are set declaratively from
the decoder, e.g. `mem_flags=[LLSC, 'ARCH_BITS & XlateFlags::LR']`
(`src/arch/riscv/isa/decoder.isa:2071`), flow through `memAccessFlags` into
the `Request` automatically, and are read with `req->getArchFlags()`
(`src/arch/riscv/tlb.cc:346`, `src/arch/riscv/pma_checker.cc:90-95`).

Verdict for our use case: fine for a 2-bit "access kind" tag (only ~2 bits
remain free), useless for a 64-bit stride value. Skip, unless all you ever
need is "this is a strided vector access".

### 2. First-class fields on `Request` (the upstream historical pattern)

`_pc`, `_taskId`, `_streamId`, `_reqInstSeqNum` were all added this way: a
private field + validity flag + accessors
(e.g. `hasInstSeqNum()/getReqInstSeqNum()`, `src/mem/request.hh:1003-1011`).
It works, but every addition edits the core `src/mem/request.hh`, which this
fork's conventions avoid when an equivalent exists. It does — see below.

### 3. The Extension framework (recommended)

`Request` inherits `Extensible<Request>` (`src/mem/request.hh:97`), which lets
any code attach a typed, reference-counted side object without touching
`request.hh` at all. The framework is `src/base/extensible.hh`:
`Extension<Target, T>` CRTP base with an auto-assigned per-type ID,
`setExtension()` (`extensible.hh:142`), `getExtension<T>()`
(`extensible.hh:182`), `removeExtension<T>()`, a mandatory `clone()`
(`extensible.hh:59`), and a worked usage example in the header comment
(`extensible.hh:80-104`). Copying a `Request` deep-clones its extensions
(`src/mem/request.hh:515-516` → `extensible.hh:123-130`).

**The in-tree exemplar is ARM MPAM** (cache partitioning IDs riding on
requests), and it demonstrates the full set → travel → read pattern:

| Step | MPAM code |
|---|---|
| Define | `class PartitionFieldExtension : public Extension<Request, PartitionFieldExtension>` with private fields + getters/setters (`src/arch/arm/mpam.hh:51-97`) |
| `clone()` | `return std::make_unique<PartitionFieldExtension>(*this);` (`src/arch/arm/mpam.cc:56-58`) |
| Set (CPU side) | `mpam::tagRequest(tc, req, ind)` builds the extension from thread state and calls `req->setExtension(ext)` (`src/arch/arm/mpam.cc:200-230`), invoked from the MMU on every translation (`src/arch/arm/mmu.cc:279`, `src/arch/arm/table_walker.cc:2682`) |
| Read (cache side) | `PartitionManager::readPacketPartitionID(PacketPtr pkt)` (`src/mem/cache/tags/partitioning_policies/partition_manager.hh:69`); the base partitioning policy explicitly points at MPAM as the reference user (`base_pp.hh:61`) |
| Build | `Source("mpam.cc", tags=['arm isa'])` (`src/arch/arm/SConscript:109`) |

`Packet` is extensible the same way (`TracingExtension`,
`src/mem/port.hh:77`), but packet extensions die with the packet; request
extensions live as long as the access — that is what we want.

## Step-by-step: an RVV stream extension

### Step 1 — define the extension (new file, no core edits)

`src/arch/riscv/insts/rvv_stream_ext.hh` (header-only ⇒ no SConscript entry
needed; add a `.cc` + `Source()` like MPAM only if implementations grow):

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

### Step 6 — build and validate

Touched files: `src/cpu/static_inst.hh` (+virtual), `src/cpu/o3/lsq.cc`
(+1 call), `src/arch/riscv/insts/vector.hh` (+overrides), plus the new
header — then rebuild `build/RISCV/gem5.opt` (per this repo's workflow, the
build is run by the user in the docker shell). Validate with the strided
microbenchmark (`tests/test-progs/custom/bin/stride1`): a `DPRINTF(HWPrefetch, ...)`
in the prefetcher printing `kind/strideBytes` under
`--debug-flags=HWPrefetch` should show `Strided, stride=16` for the
`vlse64.v` accesses (stride register value 16 in `stride1.c`).

# RVVExtension Request Extension (implemented; originally InstructionTypeExtension)

Tags every O3 data-memory `Request` with the **OpClass of the instruction
that created it**, plus — for strided vector accesses — the **rs2 register
value** (the architectural byte stride), and makes `Packet::print()` show
both. Every cache debug line then reveals what kind of access it is:

```
1353000: board.cache_hierarchy.l1d-cache-0: access for ReadReq [2868:286f] type=SimdStridedLoad rs2=16 miss
```

> Naming note: this feature was first implemented as
> `InstructionTypeExtension` (OpClass only, attached directly by the LSQ).
> It was renamed to `RVVExtension` when the rs2 field was added and the
> attach mechanism moved to the `annotateMemRequest` hook described below.
> The old header `src/mem/inst_type_ext.hh` is gone; the class now lives in
> `src/mem/rvv_ext.hh`.

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

## What this fork adds

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

## Validation

Rebuild `build/RISCV/gem5.opt` (user-run, per repo workflow), then:

```bash
build/RISCV/gem5.opt --debug-flags=Cache --debug-file=trace.log \
    rvv/riscv-rvv-se-ara-prefetcher.py [...] tests/test-progs/custom/bin/stride1
```

Expected: the two `vlse64.v` accesses appear as
`ReadReq [...] type=SimdStridedLoad rs2=16` (stride register value 16 in
`stride1.c`), the `vse64.v` as `WriteReq [...] type=SimdUnitStrideStore`
with **no** rs2 field, scalar accesses as `type=MemRead`/`type=MemWrite`,
and tick-0 `functionalAccess` lines unchanged (no `type=`).
