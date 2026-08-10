/**
 * GdpChainTable — the shared CPU-to-prefetcher channel of the Gather
 * Dataflow Prefetcher (GDP, mem/cache/prefetch/gdp.hh), built on
 * Tyche's skeleton (see cpu/tyche_table.hh for the scalar analog this
 * borrows from).
 *
 * The structures, with honest hardware widths in mind:
 *
 *  - PT (Propagation Table): per architectural vector register,
 *    whether its value descends from a tracked chain and which DCT
 *    link produced it. Updated at dispatch in program order (Tyche's
 *    PT).
 *  - DCT (Dependency Chain Table, dct_entries=8, 3-bit pointers): one
 *    link per vector instruction PC — unit-stride load heads, transform
 *    links (vsext/vsll/vadd...: op + scalar operand), and gather links
 *    (base). The transforms between producer and gather are *recorded
 *    and replayed*, not assumed away — so A[f(B[i])] with any
 *    replayable f works, including +c biases.
 *    Unlike Tyche, there is no confidence, no formed, no dense:
 *    immediates come from the encoding, scalar registers are snooped at
 *    issue (refreshed every execution), and vector loop bodies are
 *    straight-line. The table clears wholesale when full; a generation
 *    counter lets the prefetcher discard stale latched state.
 *  - Gather-link state lives IN the DCT's head rows (a once-separate
 *    link table dissolved into them — it was direct-mapped 1:1, just
 *    extra head columns): {consumer DCT ptr (-1 = unlinked), EEW}. The
 *    gather's dispatch triggers *backward propagation* — walk the
 *    back-pointers from its own link to the head and write the
 *    consumer there. Link formation takes one iteration: no training,
 *    prefetching from the second iteration.
 *
 * Classification: loads use the existing StaticInst::gdpInstInfo()
 * virtual (unit-stride EEW / indexed SEW+vs2, correct at micro-op
 * granularity); transform ops are decoded from the raw encoding via
 * StaticInst::getEMI() (OP-V major opcode), the same no-ISA-edits
 * technique the tyche port uses for scalar RV64. gem5's RISC-V
 * ExtMachInst embeds vtype's vsew, so vsext/vzext source widths are
 * known at decode.
 *
 * Known approximations (documented in DOCUMENTATION.MD): replay
 * executes transforms at 64-bit (exact for the widening index chains
 * compilers emit; SEW<64 wraparound differs), masked (vm=0) transforms
 * are recorded ignoring v0, and wrong-path dispatches update state
 * (self-healing: every gather re-walks its link).
 *
 * All state clears at every stats reset (ROI boundaries).
 */

#ifndef __CPU_GDP_TABLE_HH__
#define __CPU_GDP_TABLE_HH__

#include <array>
#include <cstdint>
#include <deque>
#include <unordered_set>
#include <vector>

#include "base/statistics.hh"
#include "base/types.hh"
#include "cpu/static_inst.hh"
#include "sim/sim_object.hh"

namespace gem5
{

struct GdpChainTableParams;

class GdpChainTable : public SimObject
{
  public:
    /** Replayable transform ops (vector chain subset) */
    enum class VOp : uint8_t
    {
        Invalid = 0,
        SExt,   // vsext.vfN: sign-extend from extFromBits
        ZExt,   // vzext.vfN
        Sll, Srl, Sra,
        Add, Sub, Rsub,
        And, Or, Xor,
        Mul,
    };

    /** One latched pipeline stage: op + its scalar operand */
    struct StageOp
    {
        VOp op = VOp::Invalid;
        /** Immediate (from the encoding) or rs1 value (snooped at
         *  issue) */
        uint64_t scalar = 0;
        /** Source width in bits for SExt/ZExt */
        unsigned extFromBits = 0;
    };

    GdpChainTable(const GdpChainTableParams &p);

    /**
     * CPU-side hook, call in dispatch (program) order for every vector
     * instruction: forward propagation (PT), head/transform/gather DCT
     * maintenance, and — on gathers — backward propagation (the head's
     * consumer-link write, done at dispatch because compilers alias
     * vd with vs2).
     */
    void dispatch(const StaticInst *si, Addr pc);

    /**
     * An indexed vector load is issuing: snoop its rs1 (target array
     * base) into its gather link. Refreshed every execution — no
     * training.
     */
    void armBase(const StaticInst *si, Addr pc, Addr base);

    /** True if the transform link at this PC needs its rs1 value
     *  (.vx form) snooped at issue. */
    bool wantsScalar(Addr pc) const;

    /** Deliver the .vx scalar operand at issue (refresh, no training) */
    void captureScalar(Addr pc, uint64_t value);

    /** Producer lookup for the prefetcher's observed accesses */
    struct ProducerInfo
    {
        bool found = false;
        bool linked = false;
        int dctPtr = -1;
        /** Producer element bytes (EEW/8): the slice width */
        unsigned elemBytes = 0;
    };
    ProducerInfo producerInfo(Addr pc) const;

    /**
     * "Chain dispatch": the replay pipeline configuration of one
     * linked producer — the transform ops in head-to-gather order plus
     * the dedicated base-adder operand. Built ON DEMAND by walking the
     * chain (consumer->head, <= dct_entries reads) when the prefetcher
     * adopts a producer at trigger time; the table stores no copy. In
     * hardware terms this models the backward-propagation walk
     * distributing each link's op straight into the pipeline stage
     * registers at the cache side — the DCT plus those stage registers
     * are the only copies of the chain. Valid only when every stage's
     * scalar and the base have been snooped at issue; operands are
     * therefore always the architecturally freshest values.
     */
    struct ChainSnapshot
    {
        bool valid = false;
        std::vector<StageOp> ops;
        /** Dedicated base-adder operand (gather rs1) */
        Addr base = 0;
        /** Table generation this snapshot belongs to */
        uint64_t gen = 0;
    };

    /**
     * The producer's pipeline configuration, walked on demand (the
     * prefetcher adopts it at trigger time and holds the only latched
     * copy).
     */
    ChainSnapshot pipelineConfig(int producer_ptr) const;

    /** Bumped on every wholesale clear (capacity or ROI reset) */
    uint64_t generation() const { return gen; }

    /**
     * Stream-page registry for stream-aware cache replacement. The
     * prefetcher registers the PHYSICAL page of every departing
     * index-array stream prefetch (it is the only agent that holds a
     * stream candidate's VA and PA together, at getPacket); a
     * replacement policy then classifies fills/touches by page. Page
     * granularity sidesteps the VA/PA mismatch: caches see PAs only,
     * and prefetch requests carry no VA.
     */
    void registerStreamPage(Addr paddr);
    bool isStreamPage(Addr paddr) const;

    /**
     * Page-level unlearning: a replacement policy that observed
     * cross-sweep reuse on this page (second touch of a demoted line)
     * removes it from the stream registry AND blocks re-registration
     * (bounded FIFO of promoted pages). Without the block, the demand
     * path re-registers the page on its very next unit-stride access
     * and unlearning is instantly undone. Blocking gives the churn
     * set its re-entry path: new fills of a promoted page insert as
     * plain LRU.
     */
    void promoteStreamPage(Addr paddr);

    /**
     * Demand-side registration (demand_stream_pages = true): called by
     * the O3 LSQ when a memory access finishes translation — the one
     * point the ISA identity (unit-stride vector load/store) and the
     * physical address are held together on the demand path. Registers
     * the access's page so stream-aware replacement works without a
     * prefetcher feeding the registry, and covers stores (output
     * streams), which no prefetcher ever walks.
     */
    void notifyDemandAccess(const StaticInst *si, Addr paddr);

    /** Wipe PT and DCT (ROI reset) */
    void resetState();

  private:
    /** Decoded classification of one vector ALU instruction */
    struct DecodedVArith
    {
        VOp op = VOp::Invalid;
        /** Scalar operand is an immediate (vs .vx rs1 at issue) */
        bool immType = true;
        int64_t imm = 0;
        unsigned extFromBits = 0;
    };
    /** Decode an OP-V transform from the raw encoding (getEMI) */
    static DecodedVArith decodeTransform(uint64_t emi);

    struct DctEntry
    {
        bool valid = false;
        bool head = false;
        bool tail = false;
        Addr pc = 0;
        VOp op = VOp::Invalid;
        /** Transform scalar / gather base */
        uint64_t scalar = 0;
        /** true = immediate from the encoding; false = .vx rs1,
         *  snooped at issue */
        bool immType = true;
        /** The stored operand has arrived: set at insert for
         *  immediates, at captureScalar for .vx rs1 values, and at
         *  armBase for gather bases */
        bool immValid = false;
        unsigned extFromBits = 0;
        /** Back-pointer to the previous chain link. 3 bits in
         *  hardware: heads have no predecessor, but every walker
         *  terminates on the head flag BEFORE following the pointer,
         *  so a head's pointer bits are don't-care (-1 here is only
         *  defensive). */
        int lastDctPtr = -1;
        // Head-row-only fields (the former Gather Link Table,
        // dissolved: it was direct-mapped 1:1 to these slots, i.e.
        // just extra columns for head rows; in a real layout they
        // fit in the head's flavor-unused field space).
        /** Head only: the gather (tail) slot this producer feeds —
         *  written by backward propagation; -1 = not linked (there
         *  is no separate linked bit). Sim walk anchor; hardware
         *  holds this only transiently during the backprop walk. */
        int consumer = -1;
        /** Head only: producer element bytes (EEW/8), the slice
         *  width, from the encoding at dispatch */
        unsigned elemBytes = 0;
    };

    /** Walk consumer->head (the backprop path) and build the
     *  pipeline configuration; called on demand by pipelineConfig() */
    ChainSnapshot chainSnapshot(int producer_ptr) const;

    struct PtEntry
    {
        bool depend = false;
        int ptr = -1;
    };

    int searchPc(Addr pc) const;
    /** Insert; on a full table, clear everything and return -1
     *  (Tyche's clear_dct semantics) */
    int dctInsert(const DctEntry &e);
    void clearAll();

    const unsigned dctEntries;
    const unsigned maxTransformStages;
    /** Register stream pages from demand unit-stride accesses (LSQ) */
    const bool demandStreamPages;

    std::vector<DctEntry> dct;
    std::array<PtEntry, 32> pt;
    uint64_t gen = 0;

    /** Stream-page registry: FIFO of the last streamPageEntries
     *  physical pages seen leaving as stream prefetches, with a set
     *  alongside for O(1) membership. Stream arrays are walked
     *  monotonically, so stale pages age out harmlessly. */
    static constexpr unsigned streamPageEntries = 64;
    static constexpr Addr streamPageShift = 12; // 4KiB pages
    std::deque<Addr> streamPageFifo;
    std::unordered_set<Addr> streamPageSet;
    /** Promoted (unlearned) pages: blocked from re-registration.
     *  Capacity is the promoted_page_entries param. */
    const unsigned promotedPageEntries;
    std::deque<Addr> promotedPageFifo;
    std::unordered_set<Addr> promotedPageSet;

    struct GdpStats : public statistics::Group
    {
        GdpStats(statistics::Group *parent);
        /** Producer heads allocated (unit-stride loads, first seen) */
        statistics::Scalar headsAllocated;
        /** Transform links inserted */
        statistics::Scalar transformsInserted;
        /** Gather links inserted */
        statistics::Scalar gathersInserted;
        /** Producer-consumer links formed (backprop completions) */
        statistics::Scalar linksFormed;
        /** Backward walks that failed to reach a head */
        statistics::Scalar linkWalksFailed;
        /** Chain breaks (undecodable op / multi-source with a tag) */
        statistics::Scalar chainsBroken;
        /** .vx scalars snooped at issue */
        statistics::Scalar scalarsCaptured;
        /** Gather bases snooped at issue */
        statistics::Scalar basesArmed;
        /** Wholesale clears on capacity */
        statistics::Scalar dctClears;
    } gdpStats;
};

} // namespace gem5

#endif // __CPU_GDP_TABLE_HH__
