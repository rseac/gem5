/**
 * VectorChainTable implementation. See vector_chain_table.hh for the design.
 * Structure follows cpu/tyche_table.cc.
 */

#include "cpu/vector_chain_table.hh"

#include <algorithm>

#include "base/bitfield.hh"
#include "base/logging.hh"
#include "base/trace.hh"
#include "cpu/reg_class.hh"
#include "debug/VectorChain.hh"
#include "params/VectorChainTable.hh"

namespace gem5
{

VectorChainTable::VectorChainTable(const VectorChainTableParams &p)
  : SimObject(p),
    dctEntries(p.dct_entries),
    maxTransformStages(p.max_transform_stages),
    consumerSlots(p.consumers_per_producer),
    demandStreamPages(p.demand_stream_pages),
    dct(p.dct_entries),
    pt(),
    monotoneArm(p.monotone_arm),
    armDistance(p.arm_distance),
    backwardSlack(p.backward_slack),
    promotedPageEntries(p.promoted_page_entries),
    tableStats(this)
{
    fatal_if(dctEntries < 3, "dct_entries must be >= 3 "
             "(head + transform + gather)");
    fatal_if(consumerSlots < 1, "consumers_per_producer must be >= 1");
    // Wipe learned state at every stats reset (ROI boundaries), like
    // the prefetchers' resetLearnedState().
    statistics::registerResetCallback([this]() { resetState(); });
}

VectorChainTable::TableStats::TableStats(statistics::Group *parent)
  : statistics::Group(parent),
    ADD_STAT(headsAllocated, statistics::units::Count::get(),
        "producer heads allocated (unit-stride vector loads)"),
    ADD_STAT(transformsInserted, statistics::units::Count::get(),
        "transform chain links inserted"),
    ADD_STAT(gathersInserted, statistics::units::Count::get(),
        "gather chain links inserted"),
    ADD_STAT(linksFormed, statistics::units::Count::get(),
        "producer-consumer links formed (backward propagation)"),
    ADD_STAT(consumerSlotsExhausted, statistics::units::Count::get(),
        "distinct gathers denied a consumer slot "
        "(consumers_per_producer cap)"),
    ADD_STAT(linkWalksFailed, statistics::units::Count::get(),
        "backward walks that did not reach a head"),
    ADD_STAT(chainsBroken, statistics::units::Count::get(),
        "chain breaks (undecodable op or ambiguous sources)"),
    ADD_STAT(scalarsCaptured, statistics::units::Count::get(),
        ".vx transform scalars snooped at issue"),
    ADD_STAT(basesArmed, statistics::units::Count::get(),
        "gather bases snooped at issue"),
    ADD_STAT(dctClears, statistics::units::Count::get(),
        "wholesale DCT clears on capacity"),
    ADD_STAT(monotoneStreamsArmed, statistics::units::Count::get(),
        "streams armed after arm_distance of monotone progress"),
    ADD_STAT(monotoneStreamsRevoked, statistics::units::Count::get(),
        "streams sticky-revoked on a backward jump (re-sweep observed)"),
    ADD_STAT(monotonePagesDropped, statistics::units::Count::get(),
        "registry pages dropped retroactively by stream revocations"),
    ADD_STAT(monotoneRegsSuppressed, statistics::units::Count::get(),
        "demand registrations suppressed (stream pre-arm or revoked)")
{
}

void
VectorChainTable::resetState()
{
    clearAll();
    // Stream Direction Table state clears only at ROI boundaries, NOT
    // in clearAll: clearAll also fires on DCT capacity overflow, and a
    // sticky revocation must survive DCT churn (the page registry
    // re-fills from the demand path within a few accesses, but a lost
    // revocation costs a whole re-sweep of demotion damage to
    // re-learn).
    streamDirTable.fill(StreamDirEntry());
    streamDirSeq = 0;
}

void
VectorChainTable::clearAll()
{
    // Head rows' consumer pointers clear with their DCT entries, so
    // no dangling 3-bit pointers survive a wholesale clear.
    std::fill(dct.begin(), dct.end(), DctEntry());
    pt.fill(PtEntry());
    streamPageFifo.clear();
    streamPageSet.clear();
    promotedPageFifo.clear();
    promotedPageSet.clear();
    gen++;
}

void
VectorChainTable::registerStreamPage(Addr paddr, Addr pcTag)
{
    const Addr page = paddr >> streamPageShift;
    // Promoted (unlearned) pages are blocked: proven cross-sweep
    // reuse must not be re-classified as a stream one access later.
    if (streamPageSet.count(page) || promotedPageSet.count(page)) {
        return;
    }
    streamPageFifo.push_back({page, pcTag});
    streamPageSet.insert(page);
    while (streamPageFifo.size() > streamPageEntries) {
        streamPageSet.erase(streamPageFifo.front().page);
        streamPageFifo.pop_front();
    }
}

bool
VectorChainTable::isStreamPage(Addr paddr) const
{
    return streamPageSet.count(paddr >> streamPageShift) != 0;
}

void
VectorChainTable::promoteStreamPage(Addr paddr)
{
    const Addr page = paddr >> streamPageShift;
    if (streamPageSet.erase(page)) {
        for (auto it = streamPageFifo.begin();
             it != streamPageFifo.end(); ++it) {
            if (it->page == page) {
                streamPageFifo.erase(it);
                break;
            }
        }
    }
    if (promotedPageSet.count(page)) {
        return;
    }
    promotedPageFifo.push_back(page);
    promotedPageSet.insert(page);
    while (promotedPageFifo.size() > promotedPageEntries) {
        promotedPageSet.erase(promotedPageFifo.front());
        promotedPageFifo.pop_front();
    }
}

void
VectorChainTable::notifyDemandAccess(const StaticInst *si, Addr pc,
                                  Addr vaddr, Addr paddr)
{
    if (!demandStreamPages) {
        return;
    }
    const auto kind = si->vecMemInfo().kind;
    if (kind != StaticInst::VecMemInfo::UnitStrideLoad &&
        kind != StaticInst::VecMemInfo::UnitStrideStore) {
        return;
    }
    if (!monotoneArm) {
        registerStreamPage(paddr, pc);
        return;
    }

    // Monotone-arm gate: track this PC's sweep in VA space (VA because
    // a VA-contiguous sweep is not PA-contiguous across page frames —
    // a PA-space limit would false-revoke at frame boundaries).
    StreamDirEntry &e = *streamDirEntry(pc, vaddr);
    if (!e.revoked) {
        if (e.dir == 0) {
            // Direction undetermined: track both extremes; latch the
            // direction once the spread clears the OoO jitter zone.
            if (vaddr > e.maxAddr) {
                e.maxAddr = vaddr;
            } else if (vaddr < e.minAddr) {
                e.minAddr = vaddr;
            }
            if (e.maxAddr - e.minAddr > backwardSlack) {
                e.dir = (e.maxAddr - e.startAddr >
                         e.startAddr - e.minAddr) ? 1 : -1;
            }
        } else if (e.dir > 0) {
            if (vaddr >= e.maxAddr) {
                e.maxAddr = vaddr;
            } else if (e.maxAddr - vaddr > backwardSlack) {
                // Below the high-water mark: this stream restarts.
                // Its lines have a future — demotion would trade
                // their hits for DRAM refetches. Sticky.
                revokeStream(e);
            }
        } else {
            if (vaddr <= e.minAddr) {
                e.minAddr = vaddr;
            } else if (vaddr - e.minAddr > backwardSlack) {
                revokeStream(e);
            }
        }
        if (!e.revoked && !e.armed &&
            e.maxAddr - e.minAddr >= armDistance) {
            e.armed = true;
            tableStats.monotoneStreamsArmed++;
        }
    }

    if (e.armed && !e.revoked) {
        registerStreamPage(paddr, pc);
    } else {
        tableStats.monotoneRegsSuppressed++;
    }
}

VectorChainTable::StreamDirEntry *
VectorChainTable::streamDirEntry(Addr pc, Addr vaddr)
{
    StreamDirEntry *victim = &streamDirTable[0];
    for (auto &e : streamDirTable) {
        if (e.valid && e.pc == pc) {
            e.lastUse = ++streamDirSeq;
            return &e;
        }
        if (!e.valid) {
            victim = &e;
        } else if (victim->valid && e.lastUse < victim->lastUse) {
            victim = &e;
        }
    }
    *victim = StreamDirEntry();
    victim->valid = true;
    victim->pc = pc;
    victim->startAddr = vaddr;
    victim->minAddr = vaddr;
    victim->maxAddr = vaddr;
    victim->lastUse = ++streamDirSeq;
    return victim;
}

void
VectorChainTable::revokeStream(StreamDirEntry &e)
{
    e.revoked = true;
    e.armed = false;
    tableStats.monotoneStreamsRevoked++;
    // Drop the pages this stream already registered: without this,
    // pages registered before the re-sweep was detected keep demoting
    // fills for the whole next pass (the jacobi timestep-2 trap).
    for (auto it = streamPageFifo.begin(); it != streamPageFifo.end();) {
        if (it->pcTag == e.pc) {
            streamPageSet.erase(it->page);
            it = streamPageFifo.erase(it);
            tableStats.monotonePagesDropped++;
        } else {
            ++it;
        }
    }
}

int
VectorChainTable::searchPc(Addr pc) const
{
    for (int i = 0; i < (int)dct.size(); i++) {
        if (dct[i].valid && dct[i].pc == pc) {
            return i;
        }
    }
    return -1;
}

int
VectorChainTable::dctInsert(const DctEntry &e)
{
    for (int i = 0; i < (int)dct.size(); i++) {
        if (!dct[i].valid) {
            dct[i] = e;
            return i;
        }
    }
    clearAll();
    tableStats.dctClears++;
    return -1; // Tyche clear_dct semantics: clear, do not insert
}

// ---------------------------------------------------------------------
// OP-V transform decode (raw encoding via StaticInst::getEMI())
// ---------------------------------------------------------------------

VectorChainTable::DecodedVArith
VectorChainTable::decodeTransform(uint64_t emi)
{
    DecodedVArith d;
    const uint32_t inst = (uint32_t)emi;
    if (bits(inst, 6, 0) != 0x57) {
        return d; // not OP-V
    }
    const unsigned funct3 = bits(inst, 14, 12);
    const unsigned funct6 = bits(inst, 31, 26);
    const unsigned rs1f = bits(inst, 19, 15);

    // OPIVI (funct3=3) / OPIVX (funct3=4): one vector source + an
    // architectural scalar — replayable. OPIVV needs a second *vector*
    // operand whose values the replay cannot know: chain breaker
    // (returns Invalid), except VXUNARY0 below.
    if (funct3 == 0x3 || funct3 == 0x4) {
        const bool is_imm = (funct3 == 0x3);
        switch (funct6) {
          case 0x00: d.op = VOp::Add; break;                   // vadd
          case 0x02:                                           // vsub
            if (!is_imm) d.op = VOp::Sub;                      // (vx only)
            break;
          case 0x03: d.op = VOp::Rsub; break;                  // vrsub
          case 0x09: d.op = VOp::And; break;                   // vand
          case 0x0a: d.op = VOp::Or; break;                    // vor
          case 0x0b: d.op = VOp::Xor; break;                   // vxor
          case 0x25: d.op = VOp::Sll; break;                   // vsll
          case 0x28: d.op = VOp::Srl; break;                   // vsrl
          case 0x29: d.op = VOp::Sra; break;                   // vsra
          default: break;
        }
        if (d.op != VOp::Invalid) {
            d.immType = is_imm;
            if (is_imm) {
                const bool shift = (funct6 == 0x25 || funct6 == 0x28 ||
                                    funct6 == 0x29);
                d.imm = shift ? (int64_t)rs1f : sext<5>(rs1f);
            }
        }
        return d;
    }

    // OPMVX (funct3=6): vmul.vx.
    if (funct3 == 0x6) {
        if (funct6 == 0x25) {
            d.op = VOp::Mul;
            d.immType = false;
        }
        return d;
    }

    // OPMVV (funct3=2): only VXUNARY0 (vsext/vzext) is a replayable
    // single-source transform; vs1 selects the variant. The source
    // width is SEW/N — SEW comes from the vtype gem5 bakes into
    // ExtMachInst (vsew at bits 37:35).
    if (funct3 == 0x2 && funct6 == 0x12) {
        const unsigned sew_bits = 8u << bits(emi, 37, 35); // 8/16/32/64
        switch (rs1f) {
          case 0x2: d.op = VOp::ZExt; d.extFromBits = sew_bits / 8; break;
          case 0x3: d.op = VOp::SExt; d.extFromBits = sew_bits / 8; break;
          case 0x4: d.op = VOp::ZExt; d.extFromBits = sew_bits / 4; break;
          case 0x5: d.op = VOp::SExt; d.extFromBits = sew_bits / 4; break;
          case 0x6: d.op = VOp::ZExt; d.extFromBits = sew_bits / 2; break;
          case 0x7: d.op = VOp::SExt; d.extFromBits = sew_bits / 2; break;
          default: break;
        }
        // Extensions carry no scalar operand: valid at insert.
        return d;
    }

    return d;
}

// ---------------------------------------------------------------------
// Dispatch: forward propagation + backward propagation (link write)
// ---------------------------------------------------------------------

void
VectorChainTable::dispatch(const StaticInst *si, Addr pc)
{
    const StaticInst::VecMemInfo info = si->vecMemInfo();

    // Architectural vector destinations.
    uint8_t dests[8];
    int ndest = 0;
    for (int i = 0; i < si->numDestRegs() && ndest < 8; i++) {
        const RegId &r = si->destRegIdx(i);
        if (r.is(VecRegClass)) {
            dests[ndest++] = r.index() & 31;
        }
    }
    if (ndest == 0) {
        return; // stores etc.: no vector register written
    }

    // Producer head: unit-stride vector load.
    if (info.kind == StaticInst::VecMemInfo::UnitStrideLoad &&
        info.elemBytes > 0) {
        int idx = searchPc(pc);
        if (idx < 0) {
            DctEntry e;
            e.valid = true;
            e.head = true;
            e.pc = pc;
            e.elemBytes = info.elemBytes;
            idx = dctInsert(e);
            if (idx >= 0) {
                tableStats.headsAllocated++;
                DPRINTF(VectorChain, "head allocated: PC %#x slot %d EEW %u\n",
                        pc, idx, info.elemBytes);
            }
        }
        for (int i = 0; i < ndest; i++) {
            pt[dests[i]] = (idx >= 0) ? PtEntry{true, idx} : PtEntry();
        }
        return;
    }

    // The gather: backward propagation NOW, at dispatch — program
    // order, and before this instruction's own destination overwrites
    // PT (compilers alias vd with vs2: "vluxei64.v v1,(a2),v1").
    if (info.kind == StaticInst::VecMemInfo::IndexedLoad &&
        info.elemBytes > 0) {
        const PtEntry src = pt[info.srcVReg & 31];
        if (src.depend && src.ptr >= 0 && src.ptr < (int)dct.size() &&
            dct[src.ptr].valid) {
            int gidx = searchPc(pc);
            if (gidx < 0) {
                DctEntry e;
                e.valid = true;
                e.tail = true;
                e.pc = pc;
                e.immType = false;
                e.immValid = false; // base arrives at issue
                e.lastDctPtr = src.ptr;
                gidx = dctInsert(e);
                if (gidx >= 0) {
                    tableStats.gathersInserted++;
                }
            } else {
                // Unlike the identity fields, provenance is dynamic:
                // chainSnapshot starts its walk here, so a reshaped
                // chain must not leave the old anchor behind.
                dct[gidx].lastDctPtr = src.ptr;
            }
            if (gidx >= 0) {
                // Walk back-pointers to the head; write the consumer link
                // there (backward propagation, one iteration).
                int ptr = src.ptr;
                unsigned hops = 0;
                while (ptr >= 0 && ptr < (int)dct.size() &&
                       dct[ptr].valid && !dct[ptr].head &&
                       hops < dctEntries) {
                    ptr = dct[ptr].lastDctPtr;
                    hops++;
                }
                if (ptr >= 0 && ptr < (int)dct.size() &&
                    dct[ptr].valid && dct[ptr].head) {
                    std::vector<int> &cons = dct[ptr].consumers;
                    const bool present =
                        std::find(cons.begin(), cons.end(), gidx)
                        != cons.end();
                    if (consumerSlots == 1) {
                        // Historical single-link behavior: the
                        // last-dispatched gather overwrites, so
                        // multi-way loops ping-pong the slot (and
                        // churn linksFormed) exactly as before.
                        if (!present) {
                            tableStats.linksFormed++;
                            DPRINTF(VectorChain, "link: producer PC %#x "
                                    "slot %d -> gather PC %#x slot %d\n",
                                    dct[ptr].pc, ptr, pc, gidx);
                        }
                        cons.assign(1, gidx);
                    } else if (!present) {
                        // Multi-way: insert-if-absent; a distinct
                        // gather beyond capacity bounces off.
                        if (cons.size() < consumerSlots) {
                            cons.push_back(gidx);
                            tableStats.linksFormed++;
                            DPRINTF(VectorChain, "link: producer PC %#x "
                                    "slot %d -> gather PC %#x slot %d "
                                    "(way %u)\n", dct[ptr].pc, ptr, pc,
                                    gidx, (unsigned)cons.size() - 1);
                        } else {
                            tableStats.consumerSlotsExhausted++;
                        }
                    }
                    // No config latch here: the prefetcher walks the
                    // chain on demand at adoption (pipelineConfig).
                    // In hardware this walk distributes each link's
                    // op into the pipeline stage registers as it
                    // passes; the table keeps no copy.
                } else {
                    tableStats.linkWalksFailed++;
                }
            }
        }
        // The gather's own output is not index provenance.
        for (int i = 0; i < ndest; i++) {
            pt[dests[i]] = PtEntry();
        }
        return;
    }

    // Vector-memory macros wrap their access micros in bookkeeping
    // micros that carry the macro's raw encoding (opcode 0x07/0x27)
    // without being the access itself: VCpyVs copies the offset
    // register into the internal register the access micros actually
    // read (vluxei's per-element micros source vtmp0, not vs2), and
    // VPinVd pins vd for renaming. They are value-transparent
    // plumbing, not transforms — VCpyVs *propagates* provenance (the
    // copy register's slot must carry the tag the gather micros look
    // up; note VecMemInternalReg0=32 masks to slot 0, coherently with
    // the micros' relative srcVReg indexing), and everything else
    // leaves the PT alone. Without this, the plumbing either broke the
    // chain (VCpyVs: undecodable op with a tagged source) or wiped the
    // tag (VPinVd: vd dest with no sources) before the gather ever
    // dispatched.
    const unsigned op7 = (unsigned)si->getEMI() & 0x7f;
    if ((op7 == 0x07 || op7 == 0x27) &&
        info.kind == StaticInst::VecMemInfo::None &&
        !si->isLoad() && !si->isStore()) {
        int src_slot = -1;
        for (int i = 0; i < si->numSrcRegs(); i++) {
            const RegId &r = si->srcRegIdx(i);
            if (r.is(VecRegClass)) {
                src_slot = r.index() & 31;
                break;
            }
        }
        if (src_slot >= 0) {
            // Copy the source's state — valid or not, so a stale tag
            // in the destination slot cannot survive.
            for (int i = 0; i < ndest; i++) {
                pt[dests[i]] = pt[src_slot];
            }
        }
        // No vector source (VPinVd): PT untouched.
        return;
    }

    // Any other memory op (strided/whole-register loads): not a
    // unit-stride index stream — invalidate.
    if (si->isLoad() || si->isStore()) {
        for (int i = 0; i < ndest; i++) {
            pt[dests[i]] = PtEntry();
        }
        return;
    }

    // Arithmetic: classify sources — exactly one non-mask vector
    // source that is not a destination (copy path), or only
    // self-sources (in-place transform). Both extend the chain
    // with a transform link if the op is replayable.
    int other_src = -1;
    int self_src = -1;
    bool ambiguous = false;
    for (int i = 0; i < si->numSrcRegs(); i++) {
        const RegId &r = si->srcRegIdx(i);
        if (!r.is(VecRegClass)) {
            continue;
        }
        const uint8_t a = r.index() & 31;
        if (a == 0) {
            continue; // v0 mask
        }
        bool is_dest = false;
        for (int d = 0; d < ndest; d++) {
            if (dests[d] == a) {
                is_dest = true;
                break;
            }
        }
        if (is_dest) {
            self_src = a;
            continue;
        }
        if (other_src == -1) {
            other_src = a;
        } else if (a != other_src) {
            ambiguous = true;
        }
    }

    int dep_reg = -1;
    if (!ambiguous && other_src >= 0) {
        dep_reg = other_src;
    } else if (!ambiguous && self_src >= 0) {
        dep_reg = self_src; // in-place: "vsll.vi v1,v1,3"
    }

    if (dep_reg >= 0 && pt[dep_reg].depend && pt[dep_reg].ptr >= 0 &&
        pt[dep_reg].ptr < (int)dct.size() && dct[pt[dep_reg].ptr].valid) {
        const DecodedVArith d = decodeTransform(si->getEMI());
        if (d.op == VOp::Invalid) {
            // A tagged value flowed into an op the pipeline cannot
            // replay: the chain breaks here.
            tableStats.chainsBroken++;
            for (int i = 0; i < ndest; i++) {
                pt[dests[i]] = PtEntry();
            }
            return;
        }
        const int last = pt[dep_reg].ptr;
        int idx = searchPc(pc);
        if (idx < 0) {
            DctEntry e;
            e.valid = true;
            e.pc = pc;
            e.op = d.op;
            e.immType = d.immType;
            e.scalar = d.immType ? (uint64_t)d.imm : 0;
            // Immediates and extensions are valid from the encoding;
            // .vx scalars arrive at issue.
            e.immValid = d.immType;
            e.extFromBits = d.extFromBits;
            e.lastDctPtr = last;
            idx = dctInsert(e);
            if (idx >= 0) {
                tableStats.transformsInserted++;
                DPRINTF(VectorChain, "transform: PC %#x slot %d op %d prev "
                        "%d\n", pc, idx, (int)d.op, last);
            }
        } else {
            DctEntry &e = dct[idx];
            // The predecessor is dynamic (a transform can be fed by a
            // different producer across instances); op/imm are static.
            e.lastDctPtr = last;
        }
        for (int i = 0; i < ndest; i++) {
            pt[dests[i]] = (idx >= 0) ? PtEntry{true, idx} : PtEntry();
        }
        return;
    }

    // No usable provenance: destinations lose theirs.
    for (int i = 0; i < ndest; i++) {
        pt[dests[i]] = PtEntry();
    }
}

// ---------------------------------------------------------------------
// Issue-time snoops
// ---------------------------------------------------------------------

void
VectorChainTable::armBase(const StaticInst *si, Addr pc, Addr base)
{
    if (si->vecMemInfo().kind != StaticInst::VecMemInfo::IndexedLoad) {
        return;
    }
    const int idx = searchPc(pc);
    if (idx < 0 || !dct[idx].tail) {
        return;
    }
    if (!dct[idx].immValid) {
        tableStats.basesArmed++;
        DPRINTF(VectorChain, "base armed: gather PC %#x base %#x\n", pc, base);
    }
    dct[idx].scalar = base;
    dct[idx].immValid = true;
    // No write-through: the on-demand walk at the next adoption reads
    // this DCT field directly, so the freshly armed base is visible
    // one trigger later with no latch to refresh.
}

VectorChainTable::ChainSnapshot
VectorChainTable::pipelineConfig(int producer_ptr, unsigned slot) const
{
    if (producer_ptr < 0 || producer_ptr >= (int)dct.size() ||
        !dct[producer_ptr].valid || !dct[producer_ptr].head) {
        return ChainSnapshot();
    }
    return chainSnapshot(producer_ptr, slot);
}

bool
VectorChainTable::wantsScalar(Addr pc) const
{
    const int idx = searchPc(pc);
    return idx >= 0 && !dct[idx].head && !dct[idx].tail &&
           !dct[idx].immType;
}

void
VectorChainTable::captureScalar(Addr pc, uint64_t value)
{
    const int idx = searchPc(pc);
    if (idx < 0 || dct[idx].head || dct[idx].tail ||
        dct[idx].immType) {
        return;
    }
    // Refreshed every issue — always the architecturally current
    // value, no stability training (contrast Tyche's conf ramp).
    dct[idx].scalar = value;
    dct[idx].immValid = true;
    tableStats.scalarsCaptured++;
}

// ---------------------------------------------------------------------
// Prefetcher-side queries
// ---------------------------------------------------------------------

VectorChainTable::ProducerInfo
VectorChainTable::producerInfo(Addr pc) const
{
    ProducerInfo out;
    const int idx = searchPc(pc);
    if (idx < 0 || !dct[idx].head) {
        return out;
    }
    out.found = true;
    out.linked = !dct[idx].consumers.empty();
    out.dctPtr = idx;
    out.elemBytes = dct[idx].elemBytes;
    out.numConsumers = dct[idx].consumers.size();
    return out;
}

VectorChainTable::ChainSnapshot
VectorChainTable::chainSnapshot(int producer_ptr, unsigned slot) const
{
    ChainSnapshot out;
    out.gen = gen;
    if (producer_ptr < 0 || producer_ptr >= (int)dct.size() ||
        !dct[producer_ptr].valid || !dct[producer_ptr].head ||
        slot >= dct[producer_ptr].consumers.size()) {
        return out;
    }
    const int c = dct[producer_ptr].consumers[slot];
    if (c < 0 || c >= (int)dct.size() || !dct[c].valid ||
        !dct[c].tail || !dct[c].immValid) {
        return out; // base not snooped yet (gather has not issued)
    }
    out.base = dct[c].scalar;

    // Collect the transform links consumer->head, then reverse into
    // pipeline (head-to-gather) order — the "chain dispatch" walk.
    std::vector<StageOp> rev;
    int ptr = dct[c].lastDctPtr;
    while (ptr != producer_ptr) {
        if (ptr < 0 || ptr >= (int)dct.size() || !dct[ptr].valid ||
            dct[ptr].head || dct[ptr].tail ||
            rev.size() >= maxTransformStages) {
            return out; // broken or over-long chain
        }
        if (!dct[ptr].immValid) {
            return out; // .vx scalar not snooped yet
        }
        rev.push_back({dct[ptr].op, dct[ptr].scalar,
                       dct[ptr].extFromBits});
        ptr = dct[ptr].lastDctPtr;
    }
    out.ops.assign(rev.rbegin(), rev.rend());
    out.valid = true;
    return out;
}

} // namespace gem5
