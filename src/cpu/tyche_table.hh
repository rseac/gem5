/**
 * TycheChainTable — the shared CPU-to-prefetcher channel of the Tyche
 * prefetcher (mem/cache/prefetch/tyche.hh), a gem5/RISC-V port of the
 * dependency-chain indirect prefetcher from the Tyche artifact
 * (~/Tyche-Artifact, ChampSim/LoongArch).
 *
 * Tyche roots dependency chains at IP-stride loads and replays the
 * *instructions* between the root load and dependent loads, so any
 * A[f(B[i])] indirection the chain's ALU can express is prefetched.
 * The three structures the artifact keeps in its decode stage
 * (ooo_cpu.cc:539-677) live here:
 *
 *  - IPT (IP-stride table, artifact IPT_L1): per-load-PC last address,
 *    stride and confidence, LRU + low-confidence replacement. Trained by
 *    the *prefetcher* on observed cache accesses (the artifact trains it
 *    in l1d_prefetcher_operate) via observeAccess(); read at *dispatch*
 *    to decide whether a load roots a chain (artifact "ip_stride_hit").
 *  - PT (Propagation Table, artifact pt[64]): per architectural integer
 *    register, whether its current value descends from a chain and the
 *    DCT link that produced it. Updated at dispatch in program order.
 *  - DCT (Dependency Chain Table, artifact DCT/DCT_ITEM): one link per
 *    instruction PC: {op, constant operand, back-pointer to the previous
 *    link, head/formed/dense flags, 2-bit constant-stability confidence}.
 *    A link replays only when need_handle() holds: formed (feeds a load)
 *    AND conf==3 (constant stable) AND dense (executes about as often as
 *    the head). The table clears wholesale when full (artifact
 *    clear_dct); the generation counter lets the prefetcher discard walk
 *    state that refers to cleared links.
 *
 * Instruction classification decodes the raw RV64 encoding obtained via
 * StaticInst::getEMI() (base + M + Zba + RVC subsets), so no ISA files
 * are touched. Only scalar instructions are fed here (the O3 dispatch
 * hook filters vectors; the VectorChainTable channel handles those).
 *
 * Deviations from the artifact (all documented in DOCUMENTATION.MD):
 *  - Constant *register* operands are sampled at issue (when the value
 *    is architecturally readable) instead of from ChampSim's decode-time
 *    shadow register file; the confidence training on the sampled values
 *    is identical. Immediate constants are trained at dispatch.
 *  - dense counting happens at dispatch, not retire (wrong-path
 *    dispatches count; the >115/256 threshold tolerates the noise).
 *  - Unknown ops with a chain dependency break the chain instead of
 *    inserting an unexecutable link (the artifact would abort in
 *    execute_alu if such a link ever replayed).
 *
 * All state clears at every stats reset (ROI boundaries), matching the
 * fork-wide prefetcher learned-state reset policy.
 */

#ifndef __CPU_TYCHE_TABLE_HH__
#define __CPU_TYCHE_TABLE_HH__

#include <array>
#include <cstdint>
#include <vector>

#include "base/statistics.hh"
#include "base/types.hh"
#include "cpu/static_inst.hh"
#include "sim/sim_object.hh"

namespace gem5
{

struct TycheChainTableParams;

class TycheChainTable : public SimObject
{
  public:
    /** Chain-link ALU operations (artifact IDM_OP, RV64 subset).
     *  W variants compute in 32 bits and sign-extend the result. */
    enum class TyOp : uint8_t
    {
        Invalid = 0,
        Load,
        AddW, AddD, SubW, SubD,
        SllW, SllD, SrlW, SrlD, SraW, SraD,
        And, Or, Xor,
        MulW, MulD,
        Sh1Add, Sh2Add, Sh3Add,
        AddUw, Sh1AddUw, Sh2AddUw, Sh3AddUw, SlliUw,
    };

    TycheChainTable(const TycheChainTableParams &p);

    /**
     * CPU-side hook: classify one dispatching scalar instruction and
     * run the artifact's decode-stage logic (PT infection, DCT link
     * insert/refresh, formed back-walk, dense counting). Call in
     * dispatch (program) order.
     */
    void dispatch(const StaticInst *si, Addr pc);

    /**
     * CPU-side hook: architectural integer register whose value the DCT
     * link at this PC needs as its constant operand, or -1. The O3 issue
     * stage reads the register and calls captureConstant().
     */
    int wantsConstant(Addr pc) const;

    /**
     * Deliver the constant register operand's value at issue. Runs the
     * artifact's stability training: same value as last time -> conf++
     * (saturating at 3), different -> conf--, and the stored constant
     * always tracks the latest value (ooo_cpu.cc:625-654).
     */
    void captureConstant(Addr pc, uint64_t value);

    /** Result of one observed access at the prefetcher. */
    struct StrideTrigger
    {
        /** IPT says prefetch addr + stride * distance (conf_trigger) */
        bool trigger = false;
        int64_t stride = 0;
    };

    /**
     * Prefetcher-side hook: train the IPT with one observed (PC, VA)
     * access and return the artifact's conf_trigger decision
     * (tyche.cc l1d_prefetcher_operate, including the LRU/replacement
     * dance and the ignore-zero-stride rule).
     */
    StrideTrigger observeAccess(Addr pc, Addr addr);

    /**
     * DCT index of the chain head at this PC if it can start a walk
     * (head with at least one need_handle() successor, the artifact's
     * search_ima gate), else -1.
     */
    int chainTrigger(Addr pc) const;

    /** Data the prefetcher needs about one DCT link. */
    struct LinkInfo
    {
        bool valid = false;
        bool isLoad = false;
        /** Load element bytes (1/2/4/8) */
        unsigned loadSize = 0;
        /** Zero-extend (lbu/lhu/lwu/FP) instead of sign-extend */
        bool loadUnsigned = false;
        /** Constant operand (imm offset for loads) */
        uint64_t src = 0;
    };
    LinkInfo link(int dct_ptr) const;

    /**
     * Successors of a link that pass need_handle(), capped at max_succ
     * (the artifact's ISQ_WRITE_PORT truncation in AGQ::update_src).
     */
    std::vector<int> successors(int dct_ptr, unsigned max_succ) const;

    /** True if the link has any need_handle() successor. */
    bool hasSuccessor(int dct_ptr) const;

    /**
     * Replay one ALU link on a propagated value (artifact
     * DCT_ITEM::execute_alu): the constant operand fills its recorded
     * position, the dynamic value the other.
     */
    uint64_t executeAlu(int dct_ptr, uint64_t dynamic_src) const;

    /**
     * One recorded ALU op, detached from the DCT so a consumer can
     * replay it after the table has been cleared (the viper scalar
     * chain copies these out at adoption).
     */
    struct ScalarOpRec
    {
        TyOp op = TyOp::Invalid;
        bool dynamicIsOp0 = true;
        uint64_t src = 0;
    };

    /** Stateless replay of one recorded op (executeAlu's ALU). */
    static uint64_t execOp(TyOp op, bool dynamic_is_op0, uint64_t src,
                           uint64_t dynamic_src);

    /**
     * Scalar-to-vector chain join (built for the removed research
     * VIPER's scalar level chain; see DOCUMENTATION.MD "VIPER: scalar
     * level chain"; no current prefetcher consumes it). The two
     * CPU-side hooks below run at the O3 IQ's VECTOR dispatch site,
     * program-ordered against the scalar dispatch() calls:
     *
     *  - noteVectorWriter: a vector instruction writing an integer
     *    register (vsetvl's rd, vcpop, vfirst...) clears that
     *    register's PT entry — stale scalar infection must not
     *    outlive the value (the vsetvl taint-kill).
     *  - noteVectorBase: a unit-stride vector load's base register
     *    provenance is looked up in the PT and, when it descends from
     *    a chain, the tail link is associated with the producer PC
     *    (the join record) and the formed backward walk runs from it
     *    — the vle plays the role a dependent scalar load plays in
     *    stock Tyche's formed marking.
     */
    void noteVectorWriter(const StaticInst *si);
    void noteVectorBase(const StaticInst *si, Addr producer_pc);

    /**
     * One level of a compiled scalar chain: the load whose VALUE the
     * level converts, plus the recorded ALU ops mapping that value to
     * the NEXT level's line address (nextImm is the next load's
     * immediate; 0 for the terminal hop, whose result is the vector
     * producer's base address).
     */
    struct ScalarLevelDesc
    {
        Addr loadPc = 0;
        unsigned loadSize = 0;
        bool loadUnsigned = false;
        std::vector<ScalarOpRec> ops;
        int64_t nextImm = 0;
    };

    /**
     * The compiled level view of the scalar chain feeding one vector
     * producer's base: level[0] is the IP-stride head. Built on
     * demand by walking the join record's tail link back to the head
     * (the promotion walk); valid only when every link on the path
     * passes needHandle() and the walk terminates at a head. The
     * caller copies it (the descs own their op records), so a later
     * DCT clear cannot dangle it; gen says which table generation it
     * was compiled from. Every link on the path is marked exported,
     * so later changes to any of them bump shapeVersion().
     */
    struct ScalarChain
    {
        bool valid = false;
        uint64_t gen = 0;
        std::vector<ScalarLevelDesc> levels;
    };
    ScalarChain scalarChainFor(Addr producer_pc, unsigned max_levels);

    /**
     * Bumped whenever the DCT is cleared (capacity or ROI reset); the
     * prefetcher stamps walk state with it and discards stale entries
     * whose links no longer exist.
     */
    uint64_t generation() const { return gen; }

    /**
     * Bumped when an EXPORTED link (one on a chain a prefetcher copied
     * out via scalarChainFor) changes what its replay would COMPUTE:
     * a register constant that re-sampled to a new value, an
     * operand-side or imm/register swap. The copy is now wrong and
     * must be dropped. The table itself keeps tracking these
     * (captureConstant, the link refresh path); this counter is how a
     * copied-out chain learns it is stale without waiting for a
     * capacity clear. Pure relabels (an inner load asserted as head
     * while its stride is confident, demoted again when it is not)
     * leave the replay untouched and do not count.
     */
    uint64_t shapeVersion() const { return shapeVer; }

    /**
     * generation() + shapeVersion(): both are monotone and every
     * clear or shape change moves exactly one of them, so a chain
     * stamped with this value is current iff the stamp still matches.
     */
    uint64_t chainVersion() const { return gen + shapeVer; }

    /**
     * Bumped when the data flow AROUND an exported chain moves: an
     * exported dependent link is fed by a different predecessor than
     * it was exported with, or a producer's join record points at a
     * different tail link. Unlike a shape change this does not make
     * the copy wrong - the same static link sits on several dynamic
     * paths (bfs: the vle base comes from the off[u] chain at a row
     * start and from e += vl on every later strip, and the row-start
     * chain stays exactly right) - it means a DIFFERENT chain may now
     * be live (bc: the next phase's head feeding the same links). So
     * a route change asks the prefetcher to re-walk and adopt whatever
     * the walk finds, keeping the compiled chain meanwhile.
     */
    uint64_t routeVersion() const { return routeVer; }

    /** Wipe IPT, PT, DCT (ROI reset) */
    void resetState();

  private:
    /** Decoded classification of one scalar RV64 instruction */
    struct DecodedInst
    {
        TyOp op = TyOp::Invalid;
        bool isLoad = false;
        /** Register operands (arch int regs; 0 = x0 / absent) */
        uint8_t rs1 = 0;
        uint8_t rs2 = 0;
        uint8_t rd = 0;
        /** Two register sources (R-type) */
        bool hasRs2 = false;
        /** Immediate operand (I-type imm / shamt / load offset) */
        int64_t imm = 0;
        /** Load writes an FP register (fld/flw): valid chain terminal,
         *  but its value never re-enters the integer PT */
        bool fpDest = false;
        unsigned loadSize = 0;
        bool loadUnsigned = false;
    };

    /** Decode the raw RV64/RVC encoding (StaticInst::getEMI()) */
    static DecodedInst decode(uint64_t emi);

    /** One DCT link (artifact DCT_ITEM) */
    struct DctItem
    {
        bool valid = false;
        bool head = false;
        /** On a path that ends in a load (set by the backward walk) */
        bool formed = false;
        /** Executes about as often as the head (>threshold per 256) */
        bool dense = true;
        /** Constant-stability confidence, replay requires 3 */
        uint8_t conf = 0;
        Addr pc = 0;
        TyOp op = TyOp::Invalid;
        /** The chain-dependent value is ALU operand 0 (rs1 side) */
        bool dynamicIsOp0 = true;
        /** Constant operand value (imm, or last sampled register) */
        uint64_t src = 0;
        /** Constant is a register still awaiting its first sample */
        bool constPending = false;
        /** Arch int reg to sample at issue (reg-constant links) */
        uint8_t constReg = 0;
        /** Constant comes from the encoding (imm), trained at dispatch */
        bool constIsImm = true;
        /** Back-pointer to the previous chain link */
        int lastDctPtr = -1;
        /** On a scalar chain the prefetcher compiled (set by a
         *  successful scalarChainFor walk); changes to an exported
         *  link's replay inputs bump shapeVersion() */
        bool exported = false;
        /** lastDctPtr at export (-1: exported as the head). A later
         *  refresh to a DIFFERENT predecessor re-routes the data flow
         *  the copy was compiled from; head<->dependent relabels
         *  with the same predecessor do not. */
        int exportedPrev = -1;
        /** dense-window execution counter (head wraps at 256) */
        uint8_t cnt = 1;
        /** seq at the last dispatch of this PC (liveness clock) */
        uint64_t lastSeen = 0;
        unsigned loadSize = 0;
        bool loadUnsigned = false;

        bool
        needHandle() const
        {
            return formed && conf == 3 && dense;
        }
    };

    /** DCT search by PC, -1 if absent */
    int searchPc(Addr pc) const;

    /**
     * Insert a link; when the table is full, clear everything (artifact
     * DCT::insert -> clear_dct) and insert into the fresh table.
     * Returns the new link's index.
     */
    int dctInsert(const DctItem &item);

    /** Clear DCT + PT and bump the generation (artifact clear_dct) */
    void clearDct();

    /** dense bookkeeping for one dispatched PC (artifact retire logic) */
    void denseCount(int dct_idx);

    /** Propagation Table entry */
    struct PtEntry
    {
        bool depend = false;
        int dctPtr = -1;
    };

    /** IP-stride table entry (artifact IPT_L1) */
    struct IptEntry
    {
        Addr ip = 0;
        Addr lastAddr = 0;
        int64_t stride = 0;
        uint8_t conf = 0;
        /** ipt_entries-1 = most recently used, 0 = next victim */
        unsigned rplcBits = 0;
    };

    /** Load PC known stride-stable at dispatch (artifact conf >= 3) */
    bool iptConfident(Addr pc) const;

    const unsigned dctEntries;
    const unsigned iptEntries;
    const unsigned denseThreshold;
    const uint64_t liveWindow;

    /** DCT-matching dispatches so far (the liveness clock) */
    uint64_t seq = 0;

    /**
     * Link idx (valid) was dispatched within liveWindow matching
     * dispatches, or liveness is off. A dead link still holds its
     * state - a re-observation revives it - but it is invisible to
     * taint propagation, join tails and promotion walks, so a table
     * that never fills cannot hand the prefetcher a chain through a
     * head that stopped running a phase ago.
     */
    bool
    linkLive(int idx) const
    {
        return liveWindow == 0 || seq - dct[idx].lastSeen <= liveWindow;
    }

    /** pt[reg] carries taint from a link that is present and live */
    bool
    taintLive(int reg) const
    {
        const int p = pt[reg].dctPtr;
        return liveWindow == 0 ||
            (p >= 0 && p < (int)dct.size() && dct[p].valid && linkLive(p));
    }

    std::vector<DctItem> dct;
    std::array<PtEntry, 32> pt;
    std::vector<IptEntry> ipt;
    uint64_t gen = 0;
    /** See shapeVersion() / routeVersion() */
    uint64_t shapeVer = 0;
    uint64_t routeVer = 0;

    /** Which replay input of an exported link changed */
    enum class ShapeChange { Const, Op };
    /** An exported link's replay inputs changed: bump shapeVer */
    void noteShapeChange(const DctItem &it, ShapeChange what);
    /** What re-routed around a compiled chain */
    enum class RouteChange { Pred, Join };
    /** Data flow moved: bump routeVer (the caller checks export) */
    void noteRouteChange(RouteChange what);

    /**
     * Join records: producer PC -> the DCT link that produced its
     * base register's value, written by noteVectorBase. Bounded FIFO
     * (a handful of vector producers per loop nest); entries from an
     * older generation are ignored at walk time.
     */
    struct JoinRec
    {
        Addr producerPc = 0;
        int tailPtr = -1;
        uint64_t gen = 0;
    };
    static constexpr unsigned joinEntries = 8;
    std::vector<JoinRec> joins;

    /** formed backward walk from one link (a load / the vle joined) */
    void markFormed(int from_ptr);

    struct TycheStats : public statistics::Group
    {
        TycheStats(statistics::Group *parent);
        /** Chain heads inserted (new IP-stride-rooted chains) */
        statistics::Scalar headsInserted;
        /** Dependent links inserted */
        statistics::Scalar linksInserted;
        /** Dependent-link refreshes (structural + imm conf training) */
        statistics::Scalar linksRefreshed;
        /** Register constants sampled at issue */
        statistics::Scalar constsCaptured;
        /** Instructions with both register sources chain-dependent */
        statistics::Scalar doubleDepends;
        /** Chain-breaking dispatches (unknown op / lost dependency) */
        statistics::Scalar chainsBroken;
        /** Wholesale DCT clears on capacity (artifact DCT_FULL) */
        statistics::Scalar dctClears;
        /** shapeVersion bumps: an exported link's replay inputs
         *  changed (total, and by which input) */
        statistics::Scalar shapeBumps;
        statistics::Scalar shapeBumpsConst;
        statistics::Scalar shapeBumpsOp;
        /** routeVersion bumps: an exported link's predecessor moved /
         *  a join record's tail moved */
        statistics::Scalar routeBumps;
        statistics::Scalar routeBumpsPred;
        statistics::Scalar routeBumpsJoin;
        /** dense-window sweeps completed */
        statistics::Scalar denseSweeps;
        /** IPT conf_trigger decisions returned to the prefetcher */
        statistics::Scalar strideTriggers;
        /** Vector-load base registers found chain-dependent (join
         *  records written by noteVectorBase) */
        statistics::Scalar vectorBasesNoted;
        /** Vector-load base registers with no tracked provenance */
        statistics::Scalar vectorBasesUntracked;
        /** Scalar chains compiled for a producer (promotion walks
         *  that succeeded) */
        statistics::Scalar scalarChainsBuilt;
        /** Promotion walks rejected (no join / stale / link not
         *  replayable / no head / too deep) */
        statistics::Scalar scalarWalksFailed;
        /** ...of which: a link on the path was not live */
        statistics::Scalar scalarWalksDead;
        /** Source-register taints ignored because their link was
         *  not live (liveness window) */
        statistics::Scalar taintsDead;
    } tycheStats;
};

} // namespace gem5

#endif // __CPU_TYCHE_TABLE_HH__
