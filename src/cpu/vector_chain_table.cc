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
    backpropMemo(p.backprop_memo),
    foldedForms(p.folded_forms),
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
    ADD_STAT(stridesCaptured, statistics::units::Count::get(),
        "strided (vlse) producer byte strides captured at commit"),
    ADD_STAT(segmentHeadsAllocated, statistics::units::Count::get(),
        "segment (vlseg) producer heads allocated"),
    ADD_STAT(segBasesCaptured, statistics::units::Count::get(),
        "segment producer macro base addresses captured at commit"),
    ADD_STAT(segmentFieldsTagged, statistics::units::Count::get(),
        "vlseg de-interleave micros that tagged a field register"),
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
        "demand registrations suppressed (stream pre-arm or revoked)"),
    ADD_STAT(backpropWalks, statistics::units::Count::get(),
        "gather dispatches that ran the backward walk"),
    ADD_STAT(backpropMemoHits, statistics::units::Count::get(),
        "gather dispatches that skipped the walk (backprop_memo)"),
    ADD_STAT(transformPredecessorMoved, statistics::units::Count::get(),
        "transform rows whose predecessor moved (memo precondition)"),
    ADD_STAT(foldsRejected, statistics::units::Count::get(),
        "walks whose chain the affine fold could not express"),
    ADD_STAT(formsPushed, statistics::units::Count::get(),
        "folded forms pushed to the consumer (folded_forms)")
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
    if (formSink) {
        formSink->invalidateAllForms();
    }
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

bool
VectorChainTable::isLinked(int gidx, int src_ptr) const
{
    if (gidx < 0 || gidx >= (int)dct.size()) {
        return false;
    }
    const DctEntry &g = dct[gidx];
    // Fail toward walking. The bit is set only by a walk that
    // reached a head AND linked, and it is cleared by a table
    // clear, a consumer eviction and any operand invalidation on
    // this chain.
    if (!g.linked) {
        return false;
    }
    return g.lastDctPtr == src_ptr; // this gather's own anchor
}

namespace {
inline bool ffIsPow2(uint64_t v) { return v && !(v & (v - 1)); }
inline unsigned ffCtz64(uint64_t v)
{
    unsigned k = 0;
    while (!(v & 1ULL)) { v >>= 1; k++; }
    return k;
}
} // anonymous namespace

bool
VectorChainTable::foldChain(const StageOp *ops, unsigned n,
                            FoldedForm &out) const
{
    // Same algebra as the engine's collapse(). It runs here so the
    // walk that already crosses these rows folds them as it goes,
    // instead of the engine walking them a second time. Ops arrive
    // in head-to-gather order.
    unsigned shift = 0;
    uint64_t bias = 0;
    bool scaled = false;
    bool extended = false;
    bool negate = false;
    out = FoldedForm();

    for (unsigned i = 0; i < n; i++) {
        const StageOp &s = ops[i];
        switch (s.op) {
          case VOp::SExt:
          case VOp::ZExt:
            if (extended || out.rshift) { return false; }
            // Extension after a narrow-width add/sub/rsub (no shift
            // yet): sext(c +/- x) == c +/- sext(x) as long as the
            // narrow arithmetic did not wrap, which index chains
            // never do in practice (TSVC s4114: vrsub.vx at e32,
            // then vsext.vf2). Kept strict for zext (not equal for
            // negative intermediates) and for anything already
            // shifted.
            if (scaled && (shift != 0 || s.op == VOp::ZExt)) {
                return false;
            }
            extended = true;
            out.extCode = FoldedForm::encodeExtWidth(s.extWidth);
            if (out.extCode == 0) { return false; } // off the SEW ladder
            out.extSigned = (s.op == VOp::SExt);
            break;
          case VOp::WMul:
          case VOp::WMulU: {
            if (scaled || extended || out.rshift) { return false; }
            if (s.scalar == 0 || !ffIsPow2(s.scalar)) { return false; }
            const unsigned k = ffCtz64(s.scalar);
            if (shift + k > 63) { return false; }
            extended = true;
            out.extCode = FoldedForm::encodeExtWidth(s.extWidth);
            if (out.extCode == 0) { return false; } // off the SEW ladder
            out.extSigned = (s.op == VOp::WMul);
            shift += k;
            bias *= s.scalar;
            scaled = true;
            break;
          }
          case VOp::Sll: {
            const unsigned k = s.scalar & 0x3f;
            if (shift + k > 63) { return false; }
            shift += k;
            bias <<= k;
            scaled = true;
            break;
          }
          case VOp::Mul: {
            if (s.scalar == 0 || !ffIsPow2(s.scalar)) { return false; }
            const unsigned k = ffCtz64(s.scalar);
            if (shift + k > 63) { return false; }
            shift += k;
            bias *= s.scalar;
            scaled = true;
            break;
          }
          case VOp::Add:  bias += s.scalar; scaled = true; break;
          case VOp::Sub:  bias -= s.scalar; scaled = true; break;
          case VOp::Rsub:
            // value' = c - value: negate the index term, fold the
            // constant. Shifts/multiplies that follow distribute over
            // the negation (-(x) << k == -(x << k)), so shift/bias
            // handling below is unchanged; bias arithmetic wraps in
            // uint64 exactly as the lane's adder does.
            bias = s.scalar - bias;
            negate = !negate;
            scaled = true;
            break;
          case VOp::Srl:
          case VOp::Sra: {
            if (scaled) { return false; }
            const bool signed_val = extended && out.extSigned;
            if ((s.op == VOp::Sra) != signed_val) { return false; }
            const unsigned k = s.scalar & 0x3f;
            if (out.rshift + k > 63) { return false; }
            out.rshift += k;
            out.rshiftSigned = signed_val;
            break;
          }
          default:
            return false;
        }
    }
    out.base = (Addr)bias;   // the chain's own constants; armBase
                             // adds the gather's base to this
    out.shift = shift;
    out.negate = negate;
    return true;
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
    // architectural scalar. OPIVV needs a second *vector* operand
    // whose values the replay cannot know: chain breaker (returns
    // Invalid), except VXUNARY0 below.
    //
    // Only the affine ops foldChain() can fold are transforms:
    // add/sub a constant, shift, multiply, extend, and (since
    // 2026-09-11) vrsub, which folds as a negated index term plus a
    // constant. vand/vor/vxor (bitwise) stay chain breakers, so a
    // tagged value flowing into one breaks the chain here
    // (chainsBroken) rather than reaching the walk and being
    // rejected there (foldsRejected). The decoder and the fold
    // therefore agree on what a chain is.
    if (funct3 == 0x3 || funct3 == 0x4) {
        const bool is_imm = (funct3 == 0x3);
        switch (funct6) {
          case 0x00: d.op = VOp::Add; break;                   // vadd
          case 0x02:                                           // vsub
            if (!is_imm) d.op = VOp::Sub;                      // (vx only)
            break;
          case 0x03: d.op = VOp::Rsub; break;                  // vrsub
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
    // OPMVX (funct3=6): the plain multiply, plus the WIDENING
    // multiplies, which fuse an extend with the multiply. Both take
    // their operand in rs1 (immType=false), so both need the .vx
    // scalar snoop before the chain is replayable.
    if (funct3 == 0x6) {
        if (funct6 == 0x25) {
            d.op = VOp::Mul;
            d.immType = false;
            return d;
        }
        // Widening: vtype SEW is the SOURCE width (destination is
        // 2*SEW), so the extension width IS sew_bits here — contrast
        // vsext.vfN below, which executes under the destination vtype
        // and therefore divides.
        const unsigned sew_bits = 8u << bits(emi, 37, 35); // 8/16/32/64
        switch (funct6) {
          case 0x38: d.op = VOp::WMulU; break;  // vwmulu.vx  (zero-extend)
          case 0x3a:                            // vwmulsu.vx (vs2 signed)
          case 0x3b: d.op = VOp::WMul;  break;  // vwmul.vx   (sign-extend)
          default: return d;                    // not a transform
        }
        d.extWidth = sew_bits;
        d.immType = false;
        return d;
    }

    // OPMVV (funct3=2): only VXUNARY0 (vsext/vzext) is a replayable
    // single-source transform; vs1 selects the variant. The source
    // width is SEW/N — SEW comes from the vtype gem5 bakes into
    // ExtMachInst (vsew at bits 37:35).
    if (funct3 == 0x2 && funct6 == 0x12) {
        const unsigned sew_bits = 8u << bits(emi, 37, 35); // 8/16/32/64
        switch (rs1f) {
          case 0x2: d.op = VOp::ZExt; d.extWidth = sew_bits / 8; break;
          case 0x3: d.op = VOp::SExt; d.extWidth = sew_bits / 8; break;
          case 0x4: d.op = VOp::ZExt; d.extWidth = sew_bits / 4; break;
          case 0x5: d.op = VOp::SExt; d.extWidth = sew_bits / 4; break;
          case 0x6: d.op = VOp::ZExt; d.extWidth = sew_bits / 2; break;
          case 0x7: d.op = VOp::SExt; d.extWidth = sew_bits / 2; break;
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
        if (r.is(VecRegClass) && r.index() < pt.size()) {
            dests[ndest++] = r.index();
        }
    }
    if (ndest == 0) {
        return; // stores etc.: no vector register written
    }

    // Producer head: unit-stride vector load, or a strided one (vlse:
    // one dispatch per element; the head is inserted on the first and
    // its vl refreshed on every one).
    const bool strided_load =
        info.kind == StaticInst::VecMemInfo::StridedLoad;
    // A unit-stride segment load (vlseg) is a unit-stride head whose
    // lines interleave nf fields; every one of its memory micros
    // carries the macro PC, so they all land on the one row. Their
    // destinations are vtmp slots: the field id is attached later,
    // by the de-interleave micro (SegmentField below).
    const bool segment_load =
        info.kind == StaticInst::VecMemInfo::SegmentLoad;
    if ((info.kind == StaticInst::VecMemInfo::UnitStrideLoad ||
         strided_load || segment_load) && info.elemBytes > 0) {
        int idx = searchPc(pc);
        if (idx < 0) {
            DctEntry e;
            e.valid = true;
            e.head = true;
            e.pc = pc;
            e.elemBytes = info.elemBytes;
            e.strided = strided_load;
            e.nfields = segment_load ? info.nfields : 0;
            idx = dctInsert(e);
            if (idx >= 0) {
                tableStats.headsAllocated++;
                if (segment_load) {
                    tableStats.segmentHeadsAllocated++;
                }
                DPRINTF(VectorChain, "head allocated: PC %#x slot %d EEW %u"
                        "%s%s nf %u\n", pc, idx, info.elemBytes,
                        strided_load ? " strided" : "",
                        segment_load ? " segment" : "",
                        (unsigned)e.nfields);
            }
        }
        if (idx >= 0) {
            dct[idx].vl = info.vl;
        }
        for (int i = 0; i < ndest; i++) {
            pt[dests[i]] = (idx >= 0) ? PtEntry{true, idx} : PtEntry();
        }
        return;
    }

    // The de-interleave micro of a vlseg: value-transparent plumbing
    // from the vtmp slots (tagged by the SegmentLoad micros above) to
    // ONE field's architectural vd. It is the point where the field is
    // known, so the provenance copy stamps it. Like VCpyVs below, a
    // source with no provenance clears the destination.
    if (info.kind == StaticInst::VecMemInfo::SegmentField) {
        PtEntry src;
        if (info.srcVReg < pt.size()) {
            src = pt[info.srcVReg];
        }
        src.field = info.field;
        for (int i = 0; i < ndest; i++) {
            pt[dests[i]] = src;
        }
        if (src.depend) {
            tableStats.segmentFieldsTagged++;
        }
        return;
    }

    // The gather: backward propagation NOW, at dispatch — program
    // order, and before this instruction's own destination overwrites
    // PT (compilers alias vd with vs2: "vluxei64.v v1,(a2),v1").
    if (info.kind == StaticInst::VecMemInfo::IndexedLoad &&
        info.elemBytes > 0) {
        if (info.srcVReg >= pt.size()) {
            return;
        }
        const PtEntry src = pt[info.srcVReg];
        if (src.depend && src.ptr >= 0 && src.ptr < (int)dct.size() &&
            dct[src.ptr].valid) {
            int gidx = searchPc(pc);
            if (gidx < 0) {
                DctEntry e;
                e.valid = true;
                e.tail = true;
                e.pc = pc;
                e.scalarType = false;
                e.scalarReady = false; // base arrives at issue
                e.lastDctPtr = src.ptr;
                e.field = src.field;
                gidx = dctInsert(e);
                if (gidx >= 0) {
                    tableStats.gathersInserted++;
                }
                // A fresh row is never linked.
            } else if (backpropMemo && isLinked(gidx, src.ptr)) {
                // The walk would re-derive what the last one latched:
                // the head is resolved, this gather's own anchor has
                // not moved, and it is still linked into that head's
                // consumers. Skip it. The anchor write below is a
                // no-op in this case (lastDctPtr == src.ptr already),
                // and no link would be formed either, because a link
                // is only formed when the gather is NOT present.
                tableStats.backpropMemoHits++;
                for (int i = 0; i < ndest; i++) {
                    pt[dests[i]] = PtEntry();
                }
                return;
            } else {
                // Unlike the identity fields, provenance is dynamic:
                // chainSnapshot starts its walk here, so a reshaped
                // chain must not leave the old anchor behind.
                dct[gidx].lastDctPtr = src.ptr;
                dct[gidx].field = src.field;
            }
            if (gidx >= 0) {
                tableStats.backpropWalks++;
                // Walk back-pointers to the head; write the consumer link
                // there (backward propagation, one iteration).
                int ptr = src.ptr;
                unsigned hops = 0;
                // Remember the rows crossed: the walk that finds the
                // head is also the only thing that knows which head a
                // transform belongs to, so stamp the answer on the way
                // rather than re-deriving it when an operand moves.
                int seen[8];
                unsigned nseen = 0;
                while (ptr >= 0 && ptr < (int)dct.size() &&
                       dct[ptr].valid && !dct[ptr].head &&
                       hops < dctEntries) {
                    if (nseen < 8) {
                        seen[nseen++] = ptr;
                    }
                    ptr = dct[ptr].lastDctPtr;
                    hops++;
                }
                if (ptr >= 0 && ptr < (int)dct.size() &&
                    dct[ptr].valid && dct[ptr].head) {
                    dct[gidx].headPtr = ptr;
                    for (unsigned k = 0; k < nseen; k++) {
                        dct[seen[k]].headPtr = ptr;
                    }
                    // The folded path does not use consumers[]:
                    // its relation lives in the head's way records,
                    // and nothing there is ever displaced. Only the
                    // op-list prefetchers still need this, so it is
                    // skipped wholesale rather than kept in step.
                    if (!foldedForms) {
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
                        if (!cons.empty() && cons[0] != gidx &&
                            cons[0] >= 0 && cons[0] < (int)dct.size()) {
                            // The evicted gather is told, so it
                            // re-walks next dispatch instead of
                            // discovering the eviction by a scan.
                            dct[cons[0]].linked = false;
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
                    }
                    // folded_forms: the walk already crosses every
                    // row of this chain, so it folds them as it goes
                    // and stores the RESULT in a head-row way. The
                    // engine then reads a form instead of walking the
                    // same rows a second time. `seen` holds the rows
                    // in gather-to-head order; the fold runs the
                    // other way.
                    bool way_ok = !foldedForms;
                    if (foldedForms) {
                        StageOp ops[8];
                        unsigned n = 0;
                        bool ready = (nseen <= maxTransformStages);
                        for (unsigned k = nseen; ready && k-- > 0; ) {
                            const DctEntry &t = dct[seen[k]];
                            if (!t.scalarReady) {
                                // A .vx operand has not been snooped
                                // yet: NOT READY rather than wrong.
                                // Leave the chain unlinked and
                                // retry on a later dispatch.
                                ready = false;
                                break;
                            }
                            if (n < 8) {
                                ops[n].op = t.op;
                                ops[n].scalar = t.scalar;
                                ops[n].extWidth = t.extWidth;
                                n++;
                            }
                        }
                        if (ready) {
                            FoldedForm f;
                            if (!foldChain(ops, n, f)) {
                                // Not an affine A[B[i]] shape. Install
                                // no way: the absence of one records
                                // the rejection, and the producer
                                // streams without converting. The
                                // chain still counts as linked, or the
                                // walk would retry a fold that can
                                // never succeed.
                                tableStats.foldsRejected++;
                                way_ok = true;
                            } else {
                                // Complete the form here. The walk
                                // folds every addition, the gather's
                                // base included, so what it pushes is
                                // usable on arrival and the engine
                                // adds nothing.
                                f.base += dct[gidx].scalar;
                                f.field = dct[gidx].field;
                                f.valid = true;
                                if (formSink) {
                                    formSink->installForm(
                                        dct[ptr].pc, gidx,
                                        dct[ptr].elemBytes,
                                        dct[ptr].nfields, f);
                                    tableStats.formsPushed++;
                                }
                                way_ok = true;
                            }
                        }
                    }
                    // Linked: the walk reached a head, linked,
                    // and (with folded_forms) has a way to live in.
                    if (way_ok) {
                        dct[gidx].linked = true;
                    }
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
    // read (vluxei's per-element micros source vtmp0..N, not vs2), and
    // VPinVd pins vd for renaming. They are value-transparent
    // plumbing, not transforms — VCpyVs *propagates* provenance into
    // the vtmp slots (pt[32..39]; the access micros report the same
    // absolute index via vecMemInfo().srcVReg), and everything else
    // leaves the PT alone. Without this, the plumbing either broke the
    // chain (VCpyVs: undecodable op with a tagged source) or wiped the
    // tag (VPinVd: vd dest with no sources) before the gather ever
    // dispatched. The vtmp slots are distinct from v0/v1 — masking
    // them together ("& 31") let one gather's VCpyVs wipe another
    // chain's live v1 tag (the s353 cross-link).
    const unsigned op7 = (unsigned)si->getEMI() & 0x7f;
    if ((op7 == 0x07 || op7 == 0x27) &&
        info.kind == StaticInst::VecMemInfo::None &&
        !si->isLoad() && !si->isStore()) {
        int src_slot = -1;
        for (int i = 0; i < si->numSrcRegs(); i++) {
            const RegId &r = si->srcRegIdx(i);
            if (r.is(VecRegClass)) {
                src_slot = r.index();
                break;
            }
        }
        if (src_slot >= 0 && src_slot < (int)pt.size()) {
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
    // vm (encoding bit 25, same raw-encoding technique as the vtype
    // extraction above): v0 as a SOURCE is the implicit mask operand
    // only on masked (vm=0) ops. On unmasked ops a v0 source is
    // ordinary data — mask-heavy loops make the register allocator
    // cycle v0 through index chains (gap_cc_sv_natural's whole
    // vle32->vsll->vluxei chain lives in v0), and skipping it
    // orphaned every such chain (transformsInserted stayed 0).
    const bool unmasked = bits((uint64_t)si->getEMI(), 25, 25);
    for (int i = 0; i < si->numSrcRegs(); i++) {
        const RegId &r = si->srcRegIdx(i);
        if (!r.is(VecRegClass)) {
            continue;
        }
        const uint8_t a = r.index();
        if (a == 0 && !unmasked) {
            continue; // the implicit v0 mask operand
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
        // The field travels with the provenance: read it before this
        // op's own destination write (in-place transforms alias).
        const uint8_t dep_field = pt[dep_reg].field;
        int idx = searchPc(pc);
        if (idx < 0) {
            DctEntry e;
            e.valid = true;
            e.pc = pc;
            e.op = d.op;
            e.scalarType = d.immType;
            e.scalar = d.immType ? (uint64_t)d.imm : 0;
            // Immediates and extensions are valid from the encoding;
            // .vx scalars arrive at issue.
            e.scalarReady = d.immType;
            e.extWidth = d.extWidth;
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
            // Counted because backprop_memo's soundness rests on this
            // never actually moving: it needs one transform PC fed by
            // two producers, which takes a non-inlined helper or a
            // control-flow merge. Recorded whether or not the memo is
            // on, so the precondition is auditable per workload.
            if (e.lastDctPtr != last) {
                tableStats.transformPredecessorMoved++;
            }
            e.lastDctPtr = last;
        }
        for (int i = 0; i < ndest; i++) {
            pt[dests[i]] = (idx >= 0) ? PtEntry{true, idx, dep_field}
                                      : PtEntry();
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
    if (!dct[idx].scalarReady) {
        tableStats.basesArmed++;
        DPRINTF(VectorChain, "base armed: gather PC %#x base %#x\n", pc, base);
    }
    // Bump the operand version only on a REAL change, so a consumer
    // memoising a compiled form re-walks when the base moves and not
    // otherwise (see operandGen()).
    if (!dct[idx].scalarReady || dct[idx].scalar != base) {
        opGen++;
        // The base is an addition operand on this chain, so a base
        // that moves is handled exactly as a transform's constant is:
        // clear this gather's linked bit and drop its record. The
        // engine learns of it through a pushed invalidate, so there
        // is no pulse to route and no slot to look up.
        dct[idx].linked = false;
        const int head = dct[idx].headPtr;
        if (foldedForms && head >= 0 && head < (int)dct.size() &&
            dct[head].valid) {
            if (formSink) {
                formSink->invalidateForm(dct[head].pc, idx);
            }
        }
        noteOperandChange(head);
    }
    dct[idx].scalar = base;
    dct[idx].scalarReady = true;
    // The base is folded in by the walk that installs the form, so
    // nothing is completed here. A base that moves clears the linked
    // bit above, and the next walk folds the new one in.
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
           !dct[idx].scalarType;
}

void
VectorChainTable::captureScalar(Addr pc, uint64_t value)
{
    const int idx = searchPc(pc);
    if (idx < 0 || dct[idx].head || dct[idx].tail ||
        dct[idx].scalarType) {
        return;
    }
    // Refreshed every issue — always the architecturally current
    // value, no stability training (contrast Tyche's conf ramp).
    // Version it on change only (see operandGen()).
    if (!dct[idx].scalarReady || dct[idx].scalar != value) {
        opGen++;
        // A transform link can sit on a chain prefix several gathers
        // share, and resolving which of them needs the forward
        // direction. Invalidate every way of the producer instead:
        // over-invalidating costs a re-walk that reproduces the same
        // form, under-invalidating would leave a stale operand folded
        // into a live address.
        noteOperandChange(dct[idx].headPtr);
        // The fold that folded THIS scalar is now stale, and only a
        // re-walk can rebuild it. This is what makes the walk gate's
        // invalidation clear load-bearing rather than optional: a
        // gather base is derived on read and needs no re-walk, but a
        // transform's constant is baked into the way record.
        const int h = dct[idx].headPtr;
        if (h >= 0 && h < (int)dct.size() && dct[h].valid) {
            for (int i = 0; i < (int)dct.size(); i++) {
                // Every gather on this head: the changed constant may
                // sit on a prefix any of them share.
                if (dct[i].valid && dct[i].tail && dct[i].headPtr == h) {
                    dct[i].linked = false;
                    if (formSink) {
                        formSink->invalidateForm(dct[h].pc, i);
                    }
                }
            }
            for (int c : dct[h].consumers) {
                if (c >= 0 && c < (int)dct.size()) {
                    dct[c].linked = false;
                }
            }
        } else {
            // Head unresolved: cannot name the gathers, so clear
            // every tail. Rare, and it fails toward re-walking.
            for (DctEntry &e : dct) {
                if (e.valid && e.tail) { e.linked = false; }
            }
        }
    }
    dct[idx].scalar = value;
    dct[idx].scalarReady = true;
    tableStats.scalarsCaptured++;
}

// ---------------------------------------------------------------------
// Prefetcher-side queries
// ---------------------------------------------------------------------

void
VectorChainTable::noteOperandChange(int dct_idx)
{
    if (dct_idx < 0 || dct_idx >= (int)dct.size() || !dct[dct_idx].valid ||
        !dct[dct_idx].head) {
        // Head unresolved (a chain that has not been walked yet, or a
        // broken one). Fail toward invalidating: a form that should
        // have been dropped and was not produces wrong addresses,
        // while an extra drop only costs a re-fold. The broadcast is
        // pushed like every other invalidate — the sink holds the
        // only copies of the forms, so it edits them directly.
        if (formSink) {
            formSink->invalidateAllForms();
        }
        return;
    }
}

VectorChainTable::ProducerInfo
VectorChainTable::producerInfo(Addr pc) const
{
    ProducerInfo out;
    const int idx = searchPc(pc);
    if (idx < 0 || !dct[idx].head) {
        return out;
    }
    if (dct[idx].strided && !dct[idx].strideValid) {
        return out; // stride not captured yet: not a usable producer
    }
    out.found = true;
    out.linked = !dct[idx].consumers.empty();
    out.dctPtr = idx;
    out.elemBytes = dct[idx].elemBytes;
    out.numConsumers = dct[idx].consumers.size();
    out.stride = dct[idx].strided ? dct[idx].stride : 0;
    out.vl = dct[idx].vl;
    out.nfields = dct[idx].nfields;
    out.segBaseValid = dct[idx].segBaseValid;
    out.segBase = dct[idx].segBase;
    return out;
}

void
VectorChainTable::captureSegBase(Addr pc, Addr base)
{
    const int idx = searchPc(pc);
    if (idx < 0 || !dct[idx].head || dct[idx].nfields < 2) {
        return;
    }
    dct[idx].segBase = base;
    dct[idx].segBaseValid = true;
    tableStats.segBasesCaptured++;
}

void
VectorChainTable::captureStride(Addr pc, uint64_t value)
{
    const int idx = searchPc(pc);
    if (idx < 0 || !dct[idx].head || !dct[idx].strided) {
        return;
    }
    dct[idx].stride = (int64_t)value;
    dct[idx].strideValid = true;
    tableStats.stridesCaptured++;
}


VectorChainTable::ChainSnapshot
VectorChainTable::chainSnapshot(int producer_ptr, unsigned slot) const
{
    ChainSnapshot out;
    out.gen = gen;
    out.opGen = opGen;
    if (producer_ptr < 0 || producer_ptr >= (int)dct.size() ||
        !dct[producer_ptr].valid || !dct[producer_ptr].head ||
        slot >= dct[producer_ptr].consumers.size()) {
        return out;
    }
    const int c = dct[producer_ptr].consumers[slot];
    if (c < 0 || c >= (int)dct.size() || !dct[c].valid ||
        !dct[c].tail || !dct[c].scalarReady) {
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
        if (!dct[ptr].scalarReady) {
            return out; // .vx scalar not snooped yet
        }
        rev.push_back({dct[ptr].op, dct[ptr].scalar,
                       dct[ptr].extWidth});
        ptr = dct[ptr].lastDctPtr;
    }
    out.ops.assign(rev.rbegin(), rev.rend());
    out.valid = true;
    return out;
}

} // namespace gem5
