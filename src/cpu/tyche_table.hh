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
     * Bumped whenever the DCT is cleared (capacity or ROI reset); the
     * prefetcher stamps walk state with it and discards stale entries
     * whose links no longer exist.
     */
    uint64_t generation() const { return gen; }

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
        /** dense-window execution counter (head wraps at 256) */
        uint8_t cnt = 1;
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

    std::vector<DctItem> dct;
    std::array<PtEntry, 32> pt;
    std::vector<IptEntry> ipt;
    uint64_t gen = 0;

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
        /** dense-window sweeps completed */
        statistics::Scalar denseSweeps;
        /** IPT conf_trigger decisions returned to the prefetcher */
        statistics::Scalar strideTriggers;
    } tycheStats;
};

} // namespace gem5

#endif // __CPU_TYCHE_TABLE_HH__
