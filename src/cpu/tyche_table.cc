/**
 * TycheChainTable implementation. See tyche_table.hh for the design and
 * ~/Tyche-Artifact for the reference code cited throughout.
 */

#include "cpu/tyche_table.hh"

#include <algorithm>

#include "base/bitfield.hh"
#include "base/logging.hh"
#include "base/trace.hh"
#include "cpu/reg_class.hh"
#include "debug/Tyche.hh"
#include "params/TycheChainTable.hh"

namespace gem5
{

TycheChainTable::TycheChainTable(const TycheChainTableParams &p)
  : SimObject(p),
    dctEntries(p.dct_entries),
    iptEntries(p.ipt_entries),
    denseThreshold(p.dense_threshold),
    liveWindow(p.live_window),
    dct(p.dct_entries),
    pt(),
    ipt(p.ipt_entries),
    tycheStats(this)
{
    fatal_if(dctEntries < 2, "dct_entries must be >= 2 (head + link)");
    fatal_if(iptEntries < 1, "ipt_entries must be >= 1");
    for (unsigned i = 0; i < iptEntries; i++) {
        ipt[i].rplcBits = i;
    }
    // Wipe learned state at every stats reset (ROI boundaries), like the
    // prefetchers' resetLearnedState().
    statistics::registerResetCallback([this]() { resetState(); });
}

TycheChainTable::TycheStats::TycheStats(statistics::Group *parent)
  : statistics::Group(parent),
    ADD_STAT(headsInserted, statistics::units::Count::get(),
        "chain heads inserted (IP-stride-rooted loads)"),
    ADD_STAT(linksInserted, statistics::units::Count::get(),
        "dependent chain links inserted"),
    ADD_STAT(linksRefreshed, statistics::units::Count::get(),
        "dependent chain link refreshes at dispatch"),
    ADD_STAT(constsCaptured, statistics::units::Count::get(),
        "register constants sampled at issue"),
    ADD_STAT(doubleDepends, statistics::units::Count::get(),
        "instructions with both register sources chain-dependent"),
    ADD_STAT(chainsBroken, statistics::units::Count::get(),
        "chain-breaking dispatches (unknown op with a dependency)"),
    ADD_STAT(dctClears, statistics::units::Count::get(),
        "wholesale DCT clears on capacity"),
    ADD_STAT(shapeBumps, statistics::units::Count::get(),
        "shape-version bumps (an exported chain link changed)"),
    ADD_STAT(shapeBumpsConst, statistics::units::Count::get(),
        "shape bumps: register constant re-sampled to a new value"),
    ADD_STAT(shapeBumpsOp, statistics::units::Count::get(),
        "shape bumps: operand side / imm-vs-register constant flip"),
    ADD_STAT(routeBumps, statistics::units::Count::get(),
        "route-version bumps (data flow moved around a compiled chain)"),
    ADD_STAT(routeBumpsPred, statistics::units::Count::get(),
        "route bumps: exported link fed by a different predecessor"),
    ADD_STAT(routeBumpsJoin, statistics::units::Count::get(),
        "route bumps: producer join record moved to a different tail"),
    ADD_STAT(denseSweeps, statistics::units::Count::get(),
        "dense-window sweeps completed"),
    ADD_STAT(strideTriggers, statistics::units::Count::get(),
        "IPT conf_trigger decisions returned to the prefetcher"),
    ADD_STAT(vectorBasesNoted, statistics::units::Count::get(),
        "vector-load base registers found chain-dependent (joins)"),
    ADD_STAT(vectorBasesUntracked, statistics::units::Count::get(),
        "vector-load base registers with no tracked provenance"),
    ADD_STAT(scalarChainsBuilt, statistics::units::Count::get(),
        "scalar chains compiled for a producer (promotion walks)"),
    ADD_STAT(scalarWalksFailed, statistics::units::Count::get(),
        "promotion walks rejected (no join / stale / not replayable)"),
    ADD_STAT(scalarWalksDead, statistics::units::Count::get(),
        "promotion walks rejected: a link on the path was not live"),
    ADD_STAT(taintsDead, statistics::units::Count::get(),
        "source taints ignored: their link was not live")
{
}

void
TycheChainTable::resetState()
{
    clearDct();
    for (unsigned i = 0; i < iptEntries; i++) {
        ipt[i] = IptEntry();
        ipt[i].rplcBits = i;
    }
}

void
TycheChainTable::clearDct()
{
    std::fill(dct.begin(), dct.end(), DctItem());
    pt.fill(PtEntry());
    joins.clear();
    gen++;
}

void
TycheChainTable::noteShapeChange(const DctItem &it, ShapeChange what)
{
    // Only links a prefetcher copied out matter: a change on an
    // unexported link is absorbed by the table's own tracking and
    // shows up in the next promotion walk.
    if (!it.exported) {
        return;
    }
    shapeVer++;
    tycheStats.shapeBumps++;
    switch (what) {
      case ShapeChange::Const: tycheStats.shapeBumpsConst++; break;
      case ShapeChange::Op:    tycheStats.shapeBumpsOp++;    break;
    }
}

void
TycheChainTable::noteRouteChange(RouteChange what)
{
    routeVer++;
    tycheStats.routeBumps++;
    switch (what) {
      case RouteChange::Pred: tycheStats.routeBumpsPred++; break;
      case RouteChange::Join: tycheStats.routeBumpsJoin++; break;
    }
}

int
TycheChainTable::searchPc(Addr pc) const
{
    for (int i = 0; i < (int)dct.size(); i++) {
        if (dct[i].valid && dct[i].pc == pc) {
            return i;
        }
    }
    return -1;
}

int
TycheChainTable::dctInsert(const DctItem &item)
{
    for (int i = 0; i < (int)dct.size(); i++) {
        if (!dct[i].valid) {
            dct[i] = item;
            dct[i].lastSeen = seq;
            return i;
        }
    }
    // Full: the artifact clears the whole table (and its PT/AGQ) and
    // does NOT insert the new item (DCT::insert returns -1). The
    // generation bump tells the prefetcher its walk state is stale.
    clearDct();
    tycheStats.dctClears++;
    return -1;
}

// ---------------------------------------------------------------------
// RV64 instruction classification (base + M + Zba + RVC subsets)
// ---------------------------------------------------------------------

namespace
{

/** Sign-extend the low 32 bits (the W-op result rule). */
inline uint64_t
sext32(uint64_t x)
{
    return (uint64_t)(int64_t)(int32_t)(uint32_t)x;
}

} // anonymous namespace

TycheChainTable::DecodedInst
TycheChainTable::decode(uint64_t emi)
{
    DecodedInst d;
    const uint32_t inst = (uint32_t)emi;
    const unsigned quad = bits(inst, 1, 0);

    if (quad == 0x3) {
        // 32-bit encoding.
        const unsigned opcode = bits(inst, 6, 0);
        const unsigned funct3 = bits(inst, 14, 12);
        const unsigned funct7 = bits(inst, 31, 25);
        d.rd = bits(inst, 11, 7);
        d.rs1 = bits(inst, 19, 15);
        d.rs2 = bits(inst, 24, 20);
        const int64_t imm_i = sext<12>(bits(inst, 31, 20));

        switch (opcode) {
          case 0x03: // LOAD
            d.isLoad = true;
            d.op = TyOp::Load;
            d.imm = imm_i;
            switch (funct3) {
              case 0x0: d.loadSize = 1; break;                          // lb
              case 0x1: d.loadSize = 2; break;                          // lh
              case 0x2: d.loadSize = 4; break;                          // lw
              case 0x3: d.loadSize = 8; break;                          // ld
              case 0x4: d.loadSize = 1; d.loadUnsigned = true; break;   // lbu
              case 0x5: d.loadSize = 2; d.loadUnsigned = true; break;   // lhu
              case 0x6: d.loadSize = 4; d.loadUnsigned = true; break;   // lwu
              default: d.op = TyOp::Invalid; d.isLoad = false; break;
            }
            break;
          case 0x07: // LOAD-FP (chain terminal; value never propagates)
            if (funct3 == 0x2 || funct3 == 0x3) {
                d.isLoad = true;
                d.op = TyOp::Load;
                d.fpDest = true;
                d.imm = imm_i;
                d.loadSize = (funct3 == 0x3) ? 8 : 4;
                d.loadUnsigned = true;
            }
            break;
          case 0x13: // OP-IMM
            d.imm = imm_i;
            switch (funct3) {
              case 0x0: d.op = TyOp::AddD; break;                     // addi
              case 0x1:
                if (bits(inst, 31, 26) == 0x00) {                     // slli
                    d.op = TyOp::SllD;
                    d.imm = bits(inst, 25, 20);
                }
                break;
              case 0x4: d.op = TyOp::Xor; break;                      // xori
              case 0x5:
                if (bits(inst, 31, 26) == 0x00) {                     // srli
                    d.op = TyOp::SrlD;
                    d.imm = bits(inst, 25, 20);
                } else if (bits(inst, 31, 26) == 0x10) {              // srai
                    d.op = TyOp::SraD;
                    d.imm = bits(inst, 25, 20);
                }
                break;
              case 0x6: d.op = TyOp::Or; break;                       // ori
              case 0x7: d.op = TyOp::And; break;                      // andi
              default: break;                                         // slti(u)
            }
            break;
          case 0x1b: // OP-IMM-32
            switch (funct3) {
              case 0x0: d.op = TyOp::AddW; d.imm = imm_i; break;      // addiw
              case 0x1:
                if (funct7 == 0x00) {                                 // slliw
                    d.op = TyOp::SllW;
                    d.imm = bits(inst, 24, 20);
                } else if (bits(inst, 31, 26) == 0x02) {              // slli.uw
                    d.op = TyOp::SlliUw;
                    d.imm = bits(inst, 25, 20);
                }
                break;
              case 0x5:
                if (funct7 == 0x00) {                                 // srliw
                    d.op = TyOp::SrlW;
                    d.imm = bits(inst, 24, 20);
                } else if (funct7 == 0x20) {                          // sraiw
                    d.op = TyOp::SraW;
                    d.imm = bits(inst, 24, 20);
                }
                break;
              default: break;
            }
            break;
          case 0x33: // OP
            d.hasRs2 = true;
            if (funct7 == 0x00) {
                switch (funct3) {
                  case 0x0: d.op = TyOp::AddD; break;
                  case 0x1: d.op = TyOp::SllD; break;
                  case 0x4: d.op = TyOp::Xor; break;
                  case 0x5: d.op = TyOp::SrlD; break;
                  case 0x6: d.op = TyOp::Or; break;
                  case 0x7: d.op = TyOp::And; break;
                  default: break;                                     // slt(u)
                }
            } else if (funct7 == 0x20) {
                if (funct3 == 0x0) d.op = TyOp::SubD;
                else if (funct3 == 0x5) d.op = TyOp::SraD;
            } else if (funct7 == 0x01) {
                if (funct3 == 0x0) d.op = TyOp::MulD;                 // mul
            } else if (funct7 == 0x10) {                              // Zba
                if (funct3 == 0x2) d.op = TyOp::Sh1Add;
                else if (funct3 == 0x4) d.op = TyOp::Sh2Add;
                else if (funct3 == 0x6) d.op = TyOp::Sh3Add;
            }
            if (d.op == TyOp::Invalid) d.hasRs2 = false;
            break;
          case 0x3b: // OP-32
            d.hasRs2 = true;
            if (funct7 == 0x00) {
                if (funct3 == 0x0) d.op = TyOp::AddW;
                else if (funct3 == 0x1) d.op = TyOp::SllW;
                else if (funct3 == 0x5) d.op = TyOp::SrlW;
            } else if (funct7 == 0x20) {
                if (funct3 == 0x0) d.op = TyOp::SubW;
                else if (funct3 == 0x5) d.op = TyOp::SraW;
            } else if (funct7 == 0x01) {
                if (funct3 == 0x0) d.op = TyOp::MulW;                 // mulw
            } else if (funct7 == 0x04) {
                if (funct3 == 0x0) d.op = TyOp::AddUw;                // add.uw
            } else if (funct7 == 0x10) {                              // Zba
                if (funct3 == 0x2) d.op = TyOp::Sh1AddUw;
                else if (funct3 == 0x4) d.op = TyOp::Sh2AddUw;
                else if (funct3 == 0x6) d.op = TyOp::Sh3AddUw;
            }
            if (d.op == TyOp::Invalid) d.hasRs2 = false;
            break;
          default:
            break; // stores, branches, LUI/AUIPC, CSR, AMO, FP-op, ...
        }
        // Unclassified: report no operands (destinations are cleared
        // generically by dispatch()).
        if (d.op == TyOp::Invalid) {
            d.rs1 = d.rs2 = d.rd = 0;
        }
        return d;
    }

    // RVC (16-bit) encoding.
    const uint16_t c = (uint16_t)inst;
    const unsigned funct3 = bits(c, 15, 13);
    if (quad == 0x0) {
        const uint8_t rdp = 8 + bits(c, 4, 2);
        const uint8_t rs1p = 8 + bits(c, 9, 7);
        if (funct3 == 0x1) { // c.fld
            d.isLoad = true;
            d.op = TyOp::Load;
            d.fpDest = true;
            d.rs1 = rs1p;
            d.imm = (bits(c, 6, 5) << 6) | (bits(c, 12, 10) << 3);
            d.loadSize = 8;
            d.loadUnsigned = true;
        } else if (funct3 == 0x2) { // c.lw
            d.isLoad = true;
            d.op = TyOp::Load;
            d.rd = rdp;
            d.rs1 = rs1p;
            d.imm = (bits(c, 5, 5) << 6) | (bits(c, 12, 10) << 3) |
                    (bits(c, 6, 6) << 2);
            d.loadSize = 4;
        } else if (funct3 == 0x3) { // c.ld
            d.isLoad = true;
            d.op = TyOp::Load;
            d.rd = rdp;
            d.rs1 = rs1p;
            d.imm = (bits(c, 6, 5) << 6) | (bits(c, 12, 10) << 3);
            d.loadSize = 8;
        }
        return d;
    }
    if (quad == 0x1) {
        const uint8_t rd = bits(c, 11, 7);
        const int64_t imm6 = sext<6>((bits(c, 12, 12) << 5) | bits(c, 6, 2));
        if (funct3 == 0x0 && rd != 0) { // c.addi
            d.op = TyOp::AddD;
            d.rd = d.rs1 = rd;
            d.imm = imm6;
        } else if (funct3 == 0x1 && rd != 0) { // c.addiw
            d.op = TyOp::AddW;
            d.rd = d.rs1 = rd;
            d.imm = imm6;
        } else if (funct3 == 0x4) {
            const uint8_t rdp = 8 + bits(c, 9, 7);
            const unsigned f2 = bits(c, 11, 10);
            if (f2 == 0x0) { // c.srli
                d.op = TyOp::SrlD;
                d.rd = d.rs1 = rdp;
                d.imm = (bits(c, 12, 12) << 5) | bits(c, 6, 2);
            } else if (f2 == 0x1) { // c.srai
                d.op = TyOp::SraD;
                d.rd = d.rs1 = rdp;
                d.imm = (bits(c, 12, 12) << 5) | bits(c, 6, 2);
            } else if (f2 == 0x2) { // c.andi
                d.op = TyOp::And;
                d.rd = d.rs1 = rdp;
                d.imm = imm6;
            } else { // register-register group
                const uint8_t rs2p = 8 + bits(c, 4, 2);
                d.rd = d.rs1 = rdp;
                d.rs2 = rs2p;
                d.hasRs2 = true;
                const unsigned f = bits(c, 6, 5);
                if (bits(c, 12, 12) == 0) {
                    if (f == 0x0) d.op = TyOp::SubD;      // c.sub
                    else if (f == 0x1) d.op = TyOp::Xor;  // c.xor
                    else if (f == 0x2) d.op = TyOp::Or;   // c.or
                    else d.op = TyOp::And;                // c.and
                } else {
                    if (f == 0x0) d.op = TyOp::SubW;      // c.subw
                    else if (f == 0x1) d.op = TyOp::AddW; // c.addw
                    else { d.hasRs2 = false; d.rs1 = d.rs2 = d.rd = 0; }
                }
            }
        }
        return d; // c.li/c.lui/branches/jumps stay Invalid
    }
    // quad == 0x2
    {
        const uint8_t rd = bits(c, 11, 7);
        const uint8_t rs2 = bits(c, 6, 2);
        if (funct3 == 0x0 && rd != 0) { // c.slli
            d.op = TyOp::SllD;
            d.rd = d.rs1 = rd;
            d.imm = (bits(c, 12, 12) << 5) | bits(c, 6, 2);
        } else if (funct3 == 0x1) { // c.fldsp
            d.isLoad = true;
            d.op = TyOp::Load;
            d.fpDest = true;
            d.rs1 = 2;
            d.imm = (bits(c, 4, 2) << 6) | (bits(c, 12, 12) << 5) |
                    (bits(c, 6, 5) << 3);
            d.loadSize = 8;
            d.loadUnsigned = true;
        } else if (funct3 == 0x2 && rd != 0) { // c.lwsp
            d.isLoad = true;
            d.op = TyOp::Load;
            d.rd = rd;
            d.rs1 = 2;
            d.imm = (bits(c, 3, 2) << 6) | (bits(c, 12, 12) << 5) |
                    (bits(c, 6, 4) << 2);
            d.loadSize = 4;
        } else if (funct3 == 0x3 && rd != 0) { // c.ldsp
            d.isLoad = true;
            d.op = TyOp::Load;
            d.rd = rd;
            d.rs1 = 2;
            d.imm = (bits(c, 4, 2) << 6) | (bits(c, 12, 12) << 5) |
                    (bits(c, 6, 5) << 3);
            d.loadSize = 8;
        } else if (funct3 == 0x4 && rd != 0 && rs2 != 0) {
            if (bits(c, 12, 12) == 0) { // c.mv: rd = rs2 (add rd,x0,rs2)
                d.op = TyOp::AddD;
                d.rd = rd;
                d.rs1 = rs2;
                d.imm = 0;
            } else { // c.add: rd = rd + rs2
                d.op = TyOp::AddD;
                d.rd = rd;
                d.rs1 = rd;
                d.rs2 = rs2;
                d.hasRs2 = true;
            }
        }
        return d;
    }
}

// ---------------------------------------------------------------------
// Dispatch-stage chain construction (artifact ooo_cpu.cc:539-677)
// ---------------------------------------------------------------------

void
TycheChainTable::dispatch(const StaticInst *si, Addr pc)
{
    // dense counting: the artifact counts every DCT-matching retire;
    // dispatch order is the closest core hook that needs no extra core
    // edits, and the >denseThreshold/256 test tolerates the wrong-path
    // noise.
    const int pre_idx = searchPc(pc);
    if (pre_idx >= 0) {
        denseCount(pre_idx);
        // Liveness clock: one tick per DCT-matching dispatch, the
        // link stamped with it (see linkLive()).
        dct[pre_idx].lastSeen = ++seq;
    }

    const uint64_t emi = si->getEMI();
    DecodedInst d = (emi != 0) ? decode(emi) : DecodedInst();

    // Integer destinations, collected generically so that instructions
    // the decoder does not classify (CSR, AMO, LUI, ...) still clear
    // their PT entries — stale infection must not outlive the value.
    uint8_t dests[4];
    int ndest = 0;
    for (int i = 0; i < si->numDestRegs() && ndest < 4; i++) {
        const RegId &r = si->destRegIdx(i);
        if (r.is(IntRegClass) && r.index() > 0 && r.index() < 32) {
            dests[ndest++] = r.index();
        }
    }
    if (ndest == 0 && !d.fpDest) {
        return; // stores, branches: nothing written, PT untouched
    }

    // Taint from a link that stopped running (a register last written
    // by a dead phase's chain and never rewritten) would make this
    // instruction both-dependent or hang it on a dead predecessor;
    // with a liveness window it reads as untracked instead.
    bool dep1 = d.rs1 != 0 && pt[d.rs1].depend;
    bool dep2 = d.hasRs2 && d.rs2 != 0 && pt[d.rs2].depend;
    if (dep1 && !taintLive(d.rs1)) {
        dep1 = false;
        tycheStats.taintsDead++;
    }
    if (dep2 && !taintLive(d.rs2)) {
        dep2 = false;
        tycheStats.taintsDead++;
    }
    const bool has_dep = dep1 || dep2;
    const bool both_dep = dep1 && dep2;
    const bool ipt_hit = d.isLoad && iptConfident(pc);

    if (both_dep) {
        tycheStats.doubleDepends++;
    }

    int link_idx = -1;

    if (ipt_hit) {
        // IP-stride load: insert or re-assert the chain head (a head
        // mistrained as a dependent link is restored here, artifact
        // ooo_cpu.cc:588-596).
        int idx = searchPc(pc);
        if (idx < 0) {
            DctItem it;
            it.valid = true;
            it.head = true;
            it.formed = true;
            it.conf = 3;
            it.pc = pc;
            it.op = TyOp::Load;
            it.loadSize = d.loadSize;
            it.loadUnsigned = d.loadUnsigned;
            it.lastDctPtr = -1;
            idx = dctInsert(it);
            if (idx >= 0) {
                tycheStats.headsInserted++;
                DPRINTF(Tyche, "head inserted: PC %#x size %u\n", pc,
                        d.loadSize);
            }
        } else {
            // Relabel only: a copied-out chain through this link
            // replays the same ops whether or not the table currently
            // calls it a head (exportedPrev keeps the compiled
            // predecessor for the refresh-path comparison).
            dct[idx].head = true;
            dct[idx].formed = true;
            dct[idx].conf = 3;
            dct[idx].lastDctPtr = -1;
            dct[idx].op = TyOp::Load;
            dct[idx].loadSize = d.loadSize;
            dct[idx].loadUnsigned = d.loadUnsigned;
        }
        link_idx = idx;
    } else if (has_dep && !both_dep) {
        if (d.op == TyOp::Invalid) {
            // Known dependency into an op the chain ALU cannot replay:
            // break the chain here instead of inserting an
            // unexecutable link (deviation; the artifact would abort
            // in execute_alu if such a link ever replayed).
            tycheStats.chainsBroken++;
        } else {
            const uint8_t dep_reg = dep1 ? d.rs1 : d.rs2;
            const int last_ptr = pt[dep_reg].dctPtr;
            const bool prev_ok = last_ptr >= 0 &&
                last_ptr < (int)dct.size() && dct[last_ptr].valid;

            // Constant operand: the immediate for I-type ops and loads;
            // the other register for R-type (x0 reads as constant 0 —
            // the artifact's source_registers[undepend_idx]==0 rule).
            const uint8_t other_reg = dep1 ? d.rs2 : d.rs1;
            const bool const_is_imm = !d.hasRs2 || other_reg == 0;
            const uint64_t imm_val =
                d.hasRs2 ? 0 : (uint64_t)d.imm;

            int idx = searchPc(pc);
            if (idx < 0 && prev_ok) {
                DctItem it;
                it.valid = true;
                it.head = false;
                it.formed = d.isLoad; // artifact: formed = is_load
                it.conf = const_is_imm ? 1 : 0;
                it.pc = pc;
                it.op = d.op;
                it.dynamicIsOp0 = dep1;
                it.constIsImm = const_is_imm;
                it.src = const_is_imm ? imm_val : 0;
                it.constPending = !const_is_imm;
                it.constReg = const_is_imm ? 0 : other_reg;
                it.lastDctPtr = last_ptr;
                it.loadSize = d.loadSize;
                it.loadUnsigned = d.loadUnsigned;
                idx = dctInsert(it);
                if (idx >= 0) {
                    tycheStats.linksInserted++;
                    DPRINTF(Tyche, "link inserted: PC %#x op %d prev %d "
                            "%s-const\n", pc, (int)d.op, last_ptr,
                            const_is_imm ? "imm" : "reg");
                    if (d.isLoad) {
                        // formed backward walk: arithmetic ancestors of
                        // a dependent load are worth replaying
                        // (ooo_cpu.cc:612-620).
                        int j = last_ptr;
                        while (j >= 0 && j < (int)dct.size() &&
                               dct[j].valid && !dct[j].formed) {
                            dct[j].formed = true;
                            j = dct[j].lastDctPtr;
                        }
                    }
                }
            } else if (idx >= 0) {
                DctItem &it = dct[idx];
                tycheStats.linksRefreshed++;
                // For a link exported as a dependent (exportedPrev
                // >= 0; a link exported as the HEAD contributes
                // neither predecessor nor op to the replay, its
                // demotion here is a relabel): an op change (operand
                // side, imm-vs-register constant, imm value) makes the
                // copy wrong - shape; a different predecessor than it
                // was exported with moves the data flow - route (the
                // copy may still be right for its own path).
                if (it.exportedPrev >= 0) {
                    if (it.dynamicIsOp0 != dep1 ||
                        (const_is_imm ? (!it.constIsImm ||
                                         it.src != imm_val)
                                      : it.constIsImm)) {
                        noteShapeChange(it, ShapeChange::Op);
                    } else if (prev_ok && last_ptr != it.exportedPrev) {
                        noteRouteChange(RouteChange::Pred);
                    }
                }
                it.head = false;
                it.dynamicIsOp0 = dep1;
                if (prev_ok) {
                    it.lastDctPtr = last_ptr;
                }
                if (const_is_imm) {
                    // Immediates (and x0) are architecturally constant:
                    // the artifact's src_regs[undepend_idx]==0 shortcut
                    // saturates them on the second sighting.
                    it.constIsImm = true;
                    it.constPending = false;
                    it.src = imm_val;
                    it.conf = 3;
                } else {
                    // Register constant: the value only exists at
                    // issue; captureConstant() runs the stability
                    // training there.
                    if (it.constIsImm) {
                        it.constIsImm = false;
                        it.constPending = true;
                        it.conf = 0;
                    }
                    it.constReg = other_reg;
                }
            }
            link_idx = idx;
        }
    }

    // PT update for the destinations (artifact ooo_cpu.cc:667-676):
    // infected when this instruction extended a chain, cleared
    // otherwise. FP destinations (fld/flw) are never tracked — an FP
    // value cannot re-enter address computation through this table.
    for (int i = 0; i < ndest; i++) {
        if (link_idx >= 0) {
            pt[dests[i]] = {true, link_idx};
        } else {
            pt[dests[i]] = PtEntry();
        }
    }
}

void
TycheChainTable::denseCount(int dct_idx)
{
    DctItem &it = dct[dct_idx];
    if (it.head && it.cnt == 0) {
        // Window boundary (the uint8 counter wrapped: 256 head
        // executions). Sweep the chain: each link is dense if it
        // executed more than denseThreshold times in the window
        // (artifact ooo_cpu.cc:1556-1580). The visited set guards
        // against back-pointer cycles a refresh could create.
        std::vector<bool> visited(dct.size(), false);
        std::vector<int> cand{dct_idx};
        visited[dct_idx] = true;
        while (!cand.empty()) {
            const int index = cand.back();
            cand.pop_back();
            for (int j = 0; j < (int)dct.size(); j++) {
                if (dct[j].valid && !dct[j].head &&
                    dct[j].lastDctPtr == index && !visited[j]) {
                    dct[j].dense = dct[j].cnt > (uint8_t)denseThreshold;
                    dct[j].cnt = 0;
                    visited[j] = true;
                    cand.push_back(j);
                }
            }
        }
        tycheStats.denseSweeps++;
    }
    if (it.head) {
        it.cnt++; // wraps at 256: the window clock
    } else if (it.cnt < 255) {
        it.cnt++;
    }
}

// ---------------------------------------------------------------------
// Issue-stage constant capture
// ---------------------------------------------------------------------

int
TycheChainTable::wantsConstant(Addr pc) const
{
    const int idx = searchPc(pc);
    if (idx < 0 || dct[idx].head || dct[idx].constIsImm) {
        return -1;
    }
    return dct[idx].constReg;
}

void
TycheChainTable::captureConstant(Addr pc, uint64_t value)
{
    const int idx = searchPc(pc);
    if (idx < 0 || dct[idx].head || dct[idx].constIsImm) {
        return;
    }
    DctItem &it = dct[idx];
    tycheStats.constsCaptured++;
    if (it.constPending) {
        it.src = value;
        it.conf = std::max<uint8_t>(it.conf, 1);
        it.constPending = false;
    } else if (it.src == value) {
        if (it.conf < 3) {
            it.conf++;
        }
    } else {
        // The stored constant always tracks the latest observation
        // (artifact ooo_cpu.cc:653), so retraining converges on the new
        // value while confidence drains and refills. A copied-out
        // chain holds the OLD value: stale it.
        noteShapeChange(it, ShapeChange::Const);
        if (it.conf > 0) {
            it.conf--;
        }
        it.src = value;
    }
}

// ---------------------------------------------------------------------
// IPT (artifact tyche.cc l1d_prefetcher_operate / update_conf)
// ---------------------------------------------------------------------

namespace
{

uint8_t
updateConf(int64_t stride, int64_t last_stride, uint8_t conf)
{
    if (stride == 0) {
        return conf;
    } else if (conf == 1) {
        return conf + 1;
    } else if (stride == last_stride) {
        return (conf < 3) ? conf + 1 : conf;
    } else {
        return (conf > 0) ? conf - 1 : 0;
    }
}

} // anonymous namespace

bool
TycheChainTable::iptConfident(Addr pc) const
{
    for (unsigned i = 0; i < iptEntries; i++) {
        if (ipt[i].conf >= 3 && ipt[i].ip == pc) {
            return true;
        }
    }
    return false;
}

TycheChainTable::StrideTrigger
TycheChainTable::observeAccess(Addr pc, Addr addr)
{
    StrideTrigger out;
    int hit = -1;
    for (unsigned i = 0; i < iptEntries; i++) {
        if (ipt[i].conf != 0 && ipt[i].ip == pc) {
            hit = i;
        }
    }

    if (hit >= 0) {
        const IptEntry e = ipt[hit]; // pre-update copy, as the artifact
        const int64_t new_stride = (int64_t)(addr - e.lastAddr);
        const bool ignore = new_stride == 0;
        const bool conf_trigger = e.conf > 1 && e.stride != 0 &&
            ((e.stride == new_stride) ||
             (e.conf >= 3 && new_stride != 0));
        if (conf_trigger) {
            out.trigger = true;
            out.stride = e.stride;
            tycheStats.strideTriggers++;
        }
        if (!ignore) {
            ipt[hit].lastAddr = addr;
            ipt[hit].conf = updateConf(new_stride, e.stride, e.conf);
            if (e.conf == 1) {
                ipt[hit].stride = new_stride;
            }
        }
        for (unsigned j = 0; j < iptEntries; j++) {
            if (ipt[j].rplcBits > e.rplcBits) {
                ipt[j].rplcBits--;
            }
        }
        ipt[hit].rplcBits = iptEntries - 1;
        return out;
    }

    // Miss: prefer re-allocating the same IP, then the LRU entry among
    // conf<2, then the plain LRU victim (artifact tyche.cc:307-356).
    int ip_idx = -1, conf0_idx = -1, rplc0_idx = -1;
    unsigned conf0_rplc = iptEntries;
    for (unsigned i = 0; i < iptEntries; i++) {
        if (ipt[i].conf < 2 && ipt[i].rplcBits < conf0_rplc) {
            conf0_idx = i;
            conf0_rplc = ipt[i].rplcBits;
        }
        if (ipt[i].ip == pc) {
            ip_idx = i;
        }
        if (ipt[i].rplcBits == 0) {
            rplc0_idx = i;
        }
    }
    const int v = (ip_idx >= 0) ? ip_idx
        : (conf0_idx >= 0) ? conf0_idx : rplc0_idx;
    ipt[v].ip = pc;
    ipt[v].lastAddr = addr;
    ipt[v].conf = 1;
    for (unsigned j = 0; j < iptEntries; j++) {
        if (ipt[j].rplcBits > ipt[v].rplcBits) {
            ipt[j].rplcBits--;
        }
    }
    ipt[v].rplcBits = iptEntries - 1;
    return out;
}

// ---------------------------------------------------------------------
// Prefetcher-side chain queries and the replay ALU
// ---------------------------------------------------------------------

int
TycheChainTable::chainTrigger(Addr pc) const
{
    const int idx = searchPc(pc);
    if (idx >= 0 && dct[idx].head && hasSuccessor(idx)) {
        return idx;
    }
    return -1;
}

TycheChainTable::LinkInfo
TycheChainTable::link(int dct_ptr) const
{
    LinkInfo out;
    if (dct_ptr < 0 || dct_ptr >= (int)dct.size() || !dct[dct_ptr].valid) {
        return out;
    }
    const DctItem &it = dct[dct_ptr];
    out.valid = true;
    out.isLoad = it.op == TyOp::Load;
    out.loadSize = it.loadSize;
    out.loadUnsigned = it.loadUnsigned;
    out.src = it.src;
    return out;
}

std::vector<int>
TycheChainTable::successors(int dct_ptr, unsigned max_succ) const
{
    std::vector<int> out;
    for (int j = 0; j < (int)dct.size() && out.size() < max_succ; j++) {
        if (dct[j].valid && !dct[j].head && dct[j].lastDctPtr == dct_ptr &&
            dct[j].needHandle()) {
            out.push_back(j);
        }
    }
    return out;
}

bool
TycheChainTable::hasSuccessor(int dct_ptr) const
{
    for (int j = 0; j < (int)dct.size(); j++) {
        if (dct[j].valid && !dct[j].head && dct[j].lastDctPtr == dct_ptr &&
            dct[j].needHandle()) {
            return true;
        }
    }
    return false;
}

uint64_t
TycheChainTable::executeAlu(int dct_ptr, uint64_t dynamic_src) const
{
    const DctItem &it = dct[dct_ptr];
    return execOp(it.op, it.dynamicIsOp0, it.src, dynamic_src);
}

uint64_t
TycheChainTable::execOp(TyOp op, bool dynamic_is_op0, uint64_t src,
                        uint64_t dynamic_src)
{
    // Operand placement follows the recorded position of the dynamic
    // (chain-propagated) value; the constant fills the other slot
    // (artifact execute_alu alu_src[] setup).
    const uint64_t a = dynamic_is_op0 ? dynamic_src : src;
    const uint64_t b = dynamic_is_op0 ? src : dynamic_src;
    switch (op) {
      case TyOp::AddW: return sext32(a + b);
      case TyOp::AddD: return a + b;
      case TyOp::SubW: return sext32(a - b);
      case TyOp::SubD: return a - b;
      case TyOp::SllW: return sext32((uint32_t)a << (b & 0x1f));
      case TyOp::SllD: return a << (b & 0x3f);
      case TyOp::SrlW: return sext32((uint32_t)a >> (b & 0x1f));
      case TyOp::SrlD: return a >> (b & 0x3f);
      case TyOp::SraW:
        return (uint64_t)(int64_t)((int32_t)(uint32_t)a >> (b & 0x1f));
      case TyOp::SraD: return (uint64_t)((int64_t)a >> (b & 0x3f));
      case TyOp::And: return a & b;
      case TyOp::Or: return a | b;
      case TyOp::Xor: return a ^ b;
      case TyOp::MulW:
        return sext32((uint64_t)((int64_t)(int32_t)(uint32_t)a *
                                 (int64_t)(int32_t)(uint32_t)b));
      case TyOp::MulD: return a * b;
      case TyOp::Sh1Add: return (a << 1) + b;
      case TyOp::Sh2Add: return (a << 2) + b;
      case TyOp::Sh3Add: return (a << 3) + b;
      case TyOp::AddUw: return (uint64_t)(uint32_t)a + b;
      case TyOp::Sh1AddUw: return ((uint64_t)(uint32_t)a << 1) + b;
      case TyOp::Sh2AddUw: return ((uint64_t)(uint32_t)a << 2) + b;
      case TyOp::Sh3AddUw: return ((uint64_t)(uint32_t)a << 3) + b;
      case TyOp::SlliUw: return (uint64_t)(uint32_t)a << (b & 0x3f);
      default: return dynamic_src; // Load/Invalid: never executed
    }
}

// ---------------------------------------------------------------------
// Scalar-to-vector chain join (viper's scalar level chain)
// ---------------------------------------------------------------------

void
TycheChainTable::markFormed(int from_ptr)
{
    int j = from_ptr;
    // Bounded by table size: back-pointers can only cycle through
    // refreshed links, and the formed test breaks any revisit.
    unsigned guard = 0;
    while (j >= 0 && j < (int)dct.size() && dct[j].valid &&
           !dct[j].formed && guard++ < dct.size()) {
        dct[j].formed = true;
        j = dct[j].lastDctPtr;
    }
}

void
TycheChainTable::noteVectorWriter(const StaticInst *si)
{
    // The vsetvl taint-kill: any integer register a vector
    // instruction writes gets untracked provenance.
    for (int i = 0; i < si->numDestRegs(); i++) {
        const RegId &r = si->destRegIdx(i);
        if (r.is(IntRegClass) && r.index() > 0 && r.index() < 32) {
            pt[r.index()] = PtEntry();
        }
    }
}

void
TycheChainTable::noteVectorBase(const StaticInst *si, Addr producer_pc)
{
    // The base is the first (and only) integer source of a
    // unit-stride vector load micro-op.
    int base_reg = -1;
    for (int i = 0; i < si->numSrcRegs(); i++) {
        const RegId &r = si->srcRegIdx(i);
        if (r.is(IntRegClass) && r.index() < 32) {
            base_reg = r.index();
            break;
        }
    }
    if (base_reg <= 0 || !pt[base_reg].depend) {
        tycheStats.vectorBasesUntracked++;
        return;
    }
    const int tail = pt[base_reg].dctPtr;
    if (tail < 0 || tail >= (int)dct.size() || !dct[tail].valid ||
        !linkLive(tail)) {
        tycheStats.vectorBasesUntracked++;
        return;
    }
    // The vle joins the chain the way a dependent scalar load does:
    // its arithmetic ancestors become worth replaying.
    markFormed(tail);
    tycheStats.vectorBasesNoted++;
    for (auto &jr : joins) {
        if (jr.producerPc == producer_pc) {
            if (jr.tailPtr != tail) {
                // The base now arrives through a different link: a
                // walk from it may find a different chain.
                noteRouteChange(RouteChange::Join);
            }
            jr.tailPtr = tail;
            jr.gen = gen;
            return;
        }
    }
    if (joins.size() >= joinEntries) {
        joins.erase(joins.begin());
    }
    joins.push_back({producer_pc, tail, gen});
    // A new join lets a producer whose earlier walk was memoized as
    // rejected try again.
    noteRouteChange(RouteChange::Join);
}

TycheChainTable::ScalarChain
TycheChainTable::scalarChainFor(Addr producer_pc, unsigned max_levels)
{
    ScalarChain out;
    const JoinRec *jr = nullptr;
    for (const auto &j : joins) {
        if (j.producerPc == producer_pc) {
            jr = &j;
            break;
        }
    }
    if (jr == nullptr || jr->gen != gen || jr->tailPtr < 0 ||
        jr->tailPtr >= (int)dct.size() || !dct[jr->tailPtr].valid) {
        tycheStats.scalarWalksFailed++;
        return out;
    }
    // Walk tail -> head, collecting the path (the promotion walk).
    std::vector<int> path;
    std::vector<bool> visited(dct.size(), false);
    int j = jr->tailPtr;
    while (j >= 0 && j < (int)dct.size() && dct[j].valid &&
           !visited[j]) {
        visited[j] = true;
        path.push_back(j);
        if (dct[j].head) {
            break;
        }
        j = dct[j].lastDctPtr;
    }
    if (path.empty() || !dct[path.back()].head) {
        tycheStats.scalarWalksFailed++;
        return out; // never reached an IP-stride head
    }
    // Every link on the path must be replayable: formed (on a path to
    // a load or the vle), constant stable, dense. Heads carry conf 3
    // and formed by construction.
    for (const int p : path) {
        if (!linkLive(p)) {
            // A head that stopped running, or a live link whose
            // predecessor pointer still names one: the chain would
            // compile but never trigger (or replay a dead phase).
            tycheStats.scalarWalksDead++;
            tycheStats.scalarWalksFailed++;
            return out;
        }
        if (!dct[p].needHandle() ||
            (!dct[p].head && dct[p].op == TyOp::Invalid)) {
            tycheStats.scalarWalksFailed++;
            return out;
        }
    }
    // Segment head -> tail at load links into level descriptors. The
    // ops between load k and load k+1 map k's VALUE to k+1's address
    // (minus the load immediate, recorded separately); the ops after
    // the last load map its value to the vector producer's base.
    std::reverse(path.begin(), path.end()); // now head-first
    for (const int p : path) {
        const DctItem &it = dct[p];
        if (it.op == TyOp::Load) {
            if (out.levels.size() >= max_levels) {
                tycheStats.scalarWalksFailed++;
                return ScalarChain();
            }
            if (!out.levels.empty()) {
                // This load's address = previous level's replayed
                // value + this immediate.
                out.levels.back().nextImm = (int64_t)it.src;
            }
            ScalarLevelDesc lvl;
            lvl.loadPc = it.pc;
            lvl.loadSize = it.loadSize;
            lvl.loadUnsigned = it.loadUnsigned;
            out.levels.push_back(std::move(lvl));
        } else {
            if (out.levels.empty()) {
                // Arithmetic before the head cannot happen (the head
                // is the walk terminal); defensive.
                tycheStats.scalarWalksFailed++;
                return ScalarChain();
            }
            out.levels.back().ops.push_back(
                {it.op, it.dynamicIsOp0, it.src});
        }
    }
    // The terminal segment's result IS the producer base: nextImm 0.
    out.levels.back().nextImm = 0;
    // The caller copies these links out; from here on their changes
    // must reach it (shapeVersion).
    for (const int p : path) {
        dct[p].exported = true;
        dct[p].exportedPrev = dct[p].head ? -1 : dct[p].lastDctPtr;
    }
    out.valid = true;
    out.gen = gen;
    tycheStats.scalarChainsBuilt++;
    return out;
}

} // namespace gem5
