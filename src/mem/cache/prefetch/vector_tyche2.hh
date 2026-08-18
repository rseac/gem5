/**
 * VTyche — Vector Tyche, an A[B[i]] gather prefetcher (cache side).
 *
 * A hybrid of GDP (mem/cache/prefetch/gdp.hh) and IMP
 * (mem/cache/prefetch/indirect_memory.hh): it keeps GDP's
 * architectural discovery — the Tyche-derived CPU-side chain table
 * (cpu/vector_chain_table.hh) names the producer load, links it to its gather
 * in one iteration, and snoops the gather's base register — but
 * replaces GDP's transform-replay pipeline with IMP's linear equation
 *
 *     target = base + (index << shift)
 *
 * evaluated by a shift-and-add lane per sliced index, all lanes in
 * parallel. The chain is not replayed at runtime; it is *collapsed
 * once*, at adoption, into that (base, shift) pair.
 *
 * Why the collapse is lossless for A[B[i]]. RVV indexed loads take
 * BYTE offsets, so a source-level A[B[i]] compiles to
 * vle32 -> vsext.vf2 -> vsll.vi #k -> vluxei64, i.e. the chain GDP
 * would replay is exactly an extend, a shift, and the gather's base
 * add. Folding it yields the same addresses GDP computes, with no
 * per-stage pipeline registers and no serial per-element drain. What
 * is given up is everything beyond an affine index->address map:
 * A[f(B[i])] with a multiply by a non-power-of-2, a right shift, a
 * reversed subtract or a logic op is rejected outright (chainsRejected)
 * rather than approximated, so the difference against GDP on the same
 * kernel is exactly the transform pipeline's contribution.
 *
 * The accepted algebra (collapse(), vector_tyche.cc): at most one
 * leading vsext/vzext (kept as the slice's extension width and sign);
 * then any number of vsll and power-of-2 vmul, which accumulate into
 * `shift`; and any number of scalar vadd/vsub, which accumulate into a
 * bias folded into `base`. So A[B[i] + c] and A[B[i] * 8] collapse too
 * — the base adder and the shifter are already there, folding costs
 * nothing. Everything else breaks the form.
 *
 * The runtime is GDP's, unchanged in structure so the two are directly
 * comparable:
 *
 *  - every observed access at a producer PC advances that producer's
 *    architectural stream walk and prefetches the index array
 *    streaming_distance lines ahead (confidence-free: unit-stride
 *    identity comes from the opcode);
 *  - index-line data is captured by RANGE, not per-line tags: a
 *    prefetch about to issue (getPacket peeks the deferred packet,
 *    whose PrefetchInfo holds the VA and whose pkt the translated PA)
 *    or a demand miss inside a configured producer's stream window
 *    registers that line's PA in the Index Routing Table (IRT), and
 *    notifyFill matches the PA against it;
 *  - fills LATCH THE RAW PAYLOAD into a slice buffer, exactly as GDP
 *    does (slice_buffer_entries, same name by design): the staging,
 *    its occupancy semantics, and its lazy evaluation are identical,
 *    so the two designs differ ONLY in the drain's compute engine.
 *  - at the drain, ONE LINE CONVERTS PER EVENT: the whole line is
 *    sliced at the producer's EEW and every element goes through its
 *    own shift-and-add lane simultaneously (blkSize / elemBytes lanes
 *    — 16 for a 64 B line of 32-bit indices) into an output latch of
 *    line-deduplicated target addresses. GDP walks the same payload
 *    through its replay pipeline at pipelines_per_gather ELEMENTS per
 *    event instead; the drain compute width is the entire structural
 *    difference. The lane array is one line wide, so a second
 *    conversion waits for the next event (emissionLineLimited).
 *  - emission is demand-clocked and STALLS ON FULL: the latch
 *    releases into free prefetch-queue slots at the next observed
 *    access (which also supplies the translation context fills lack),
 *    and what does not fit waits latched; the array cannot convert
 *    the next line until its output latch drains. drain_floor bounds
 *    the displacement when the queue is persistently full; without it
 *    the emission wedges exactly when miss pressure — and therefore
 *    the need — is highest, since gem5's queue drains only on cache
 *    pulls. Set it to 0 for a pure stall.
 *  - optional CROSS-LINE DEDUP (dedup_buffer_size = N > 0): every
 *    target line is checked against the lines emitted by that
 *    producer's last N processed index lines; matches drop
 *    (targetsCrossDeduplicated) instead of re-entering the queue.
 *    The one-line-per-event conversion already serializes lines, so
 *    N sets only the window depth — the CAM the serial cadence pays
 *    for. This closes the redundancy the per-batch dedup cannot see:
 *    neighboring index lines re-targeting the same data lines (~24%
 *    of candidates were already-resident on poisson3Db).
 *    First-emit-wins: a dropped target was pushed by an earlier line
 *    still inside the window, never silently lost.
 *
 * Load shedding is chunk-granular at fill admission, as in GDP: each
 * pipeline has one slice buffer of slice_buffer_entries raw lines,
 * and a fill arriving with it full is dropped whole (bufferBusyDrops)
 * — one uncovered line stalls the entire gather anyway. A staleness
 * abort discards lines whose index line the producer's walk cursor
 * has already passed (stalenessAborts) — for buffered lines before
 * any compute is paid, matching GDP's laziness — so a stall never
 * rots into a late prefetch.
 *
 * No confidence and no kill switch: base and shift are architectural,
 * refreshed from the freshest snooped operands every adoption, so
 * systematic mispredictions cannot arise from the A[B[i]] shape
 * itself.
 */

#ifndef __MEM_CACHE_PREFETCH_VECTOR_TYCHE2_HH__
#define __MEM_CACHE_PREFETCH_VECTOR_TYCHE2_HH__

#include <deque>
#include <list>
#include <memory>
#include <unordered_map>
#include <vector>

#include "base/statistics.hh"
#include "cpu/vector_chain_table.hh"
#include "mem/cache/prefetch/queued.hh"

namespace gem5
{

struct VectorTyche2PrefetcherParams;

namespace prefetch
{

class VectorTyche2 : public Queued
{
    /** The CPU-side chain/link table (shared SimObject, same one GDP
     *  uses — the discovery half is identical) */
    VectorChainTable *const tbl;
    /** DEEP index/stream staging distance (index_distance): how far
     *  ahead of the cursor index lines are fetched and captured. Deep
     *  fetches stage in the L2 and the slice buffers; nothing about
     *  them needs to survive in the L1. */
    const int streamingDistance;
    /** NEAR release distance (release_distance, in chunks): converted
     *  targets are released -- and stream lines re-promoted into the
     *  L1 -- only when the cursor is within this many chunks of their
     *  index line. Decouples metadata lead from data residency. */
    const int releaseDistance;
    /** Max target addresses emitted per drain firing (0 = unlimited).
     *  Spreads a due batch across cycles instead of dumping it at the
     *  release-gate edge, smoothing queue/MSHR contention. */
    const unsigned releasePace;
    /** On a fresh/reset window, jump the walk to the frontier instead
     *  of burst-filling the near window (see Prefetcher.py) */
    const bool streamStartAtDistance;
    /** Ablation: stream the index arrays but never capture/convert */
    const bool streamOnly;
    /** Captured raw index lines each pipeline's slice buffer holds
     *  (GDP's slice_buffer_entries: the staging is identical) */
    const unsigned sliceBufferEntries;
    /** Concurrently configured producer pipelines */
    const unsigned pipelines;
    /** Index Routing Table capacity (registered awaiting-fill lines) */
    const unsigned irtEntries;
    /** Minimum emissions per access event when the queue is full */
    const int drainFloor;
    /** Cross-line dedup window in index lines; 0 disables */
    const unsigned dedupBufferSize;

    /**
     * The collapsed chain: target = base + (extend(elem) << shift).
     *
     * This replaces GDP's ChainSnapshot stage list. In hardware it is
     * one shifter and one adder per lane plus the slice's extension
     * width — the whole "pipeline configuration" is two operands.
     */
    struct LinearForm
    {
        bool valid = false;
        /** Gather base register plus every folded constant bias */
        Addr base = 0;
        /** Folded left shifts and power-of-2 multiplies */
        unsigned shift = 0;
        /** Element extension width in bits; 0 = take the slice raw */
        unsigned extBits = 0;
        /** Sign-extend (vsext) rather than zero-extend (vzext) */
        bool extSigned = false;
        /** Chain-table generation this form was collapsed from */
        uint64_t gen = 0;
    };

    /**
     * One captured index line, latched raw at the fill. The slice
     * buffer stages payloads exactly as GDP's does; conversion waits
     * for the drain, so staleness aborts flush buffered lines before
     * any compute is paid.
     */
    struct CapturedLine
    {
        /** Line VA of the index data, for the staleness abort */
        Addr lineVaddr = 0;
        /** Raw payload, sliced at the producer's EEW at conversion */
        std::vector<uint8_t> data;
    };

    /**
     * The lane array's output latch: one converted line's target
     * addresses, already line-deduplicated, draining into queue room
     * across events. One latch per pipeline — the array cannot start
     * the next line until it empties.
     */
    struct ConvertedLine
    {
        /** Line VA of the index data, for the staleness abort */
        Addr lineVaddr = 0;
        /** Distinct target lines, as element-precise addresses */
        std::vector<Addr> targets;
        /** Next target to emit (the emission cursor) */
        unsigned next = 0;
        /** Target lines actually pushed — this line's dedup-window
         *  contribution, built across mid-batch stalls (only
         *  maintained when dedup_buffer_size > 0) */
        std::vector<Addr> emitted;
    };

    /** Per-producer runtime state (stream walk + collapsed form) */
    struct SttEntry
    {
        Addr lastAddr = 0;
        bool valid = false;
        /** Stream high-water mark (exclusive end of issued window) */
        Addr limitAddr = 0;
        /** Near-promotion high-water mark (exclusive): lines below it
         *  have been re-emitted for their just-in-time L1 copy */
        Addr promoteLimitAddr = 0;
        /** Byte size of the latest chunk access (the release gate's
         *  chunk unit) */
        unsigned lastSize = 0;
        /** The adopted linear form */
        LinearForm form;
        bool configured = false;
        /** Slice width latched with the form (producer EEW/8) */
        unsigned elemBytes = 0;
        /** Captured raw index lines awaiting conversion */
        std::deque<CapturedLine> sliceBuffer;
        /** The lane array's output latch (valid when latchValid) */
        ConvertedLine latch;
        bool latchValid = false;
        /** Emitted target lines of the last dedup_buffer_size
         *  processed index lines, oldest first (the cross-line CAM;
         *  empty when dedup_buffer_size = 0) */
        std::deque<std::vector<Addr>> dedupWindow;
    };
    std::unordered_map<Addr, SttEntry> streamTrackingTable;
    /** Producers currently holding a pipeline (vs pipelines cap) */
    unsigned configuredCount = 0;

    /** Index Routing Table entry: an index line registered for
     *  capture, with producerPC as the routing destination */
    struct IrtEntry
    {
        Addr linePaddr;
        Addr lineVaddr;
        Addr producerPC;
        bool secure;
    };
    std::list<IrtEntry> indexRoutingTable;

    struct VTycheStats : public statistics::Group
    {
        VTycheStats(statistics::Group *parent);
        /** Chunk events at producer PCs */
        statistics::Scalar chunksObserved;
        /** Index-array stream candidates emitted */
        statistics::Scalar streamCandidates;
        /** Linear forms adopted at trigger */
        statistics::Scalar formsAdopted;
        /** Linked triggers whose chain was not yet snoopable */
        statistics::Scalar chainNotReady;
        /** Chains rejected: not collapsible to base + (idx << shift) */
        statistics::Scalar chainsRejected;
        /** Linked producers denied a pipeline (pipelines cap) */
        statistics::Scalar pipelinesSaturated;
        /** Lines registered for capture (prefetch-window path) */
        statistics::Scalar capturesRegistered;
        /** Lines registered for capture (demand-miss path) */
        statistics::Scalar capturesRegisteredMiss;
        /** Captured index-line fills converted */
        statistics::Scalar fillsCaptured;
        /** Fills dropped: slice buffer full */
        statistics::Scalar bufferBusyDrops;
        /** Fills dropped: form stale (table cleared) */
        statistics::Scalar staleConfigs;
        /** Elements pushed through a shift-and-add lane */
        statistics::Scalar elementsConverted;
        /** Indirect target candidates generated */
        statistics::Scalar targetsGenerated;
        /** Targets dropped by the canonical-address filter */
        statistics::Scalar targetsFiltered;
        /** Targets dropped as duplicate lines within one batch */
        statistics::Scalar targetsDeduplicated;
        /** Targets dropped: line emitted within the dedup window */
        statistics::Scalar targetsCrossDeduplicated;
        /** Batches flushed: walk cursor passed their index line */
        statistics::Scalar stalenessAborts;
        /** Emission events paused by a full queue (the stall firing) */
        statistics::Scalar emissionDeferred;
        /** Events where the array's one-line conversion slot ended
         *  with further lines still buffered */
        statistics::Scalar emissionLineLimited;
        /** Self-clocked drain firings that ran the emission engine */
        statistics::Scalar selfDrainTicks;
        /** Near-window re-promotions issued (just-in-time L1 copies) */
        statistics::Scalar promotionsIssued;
        /** Buffered lines held back by the release gate at a drain */
        statistics::Scalar releaseHeld;
        /** Drain firings ended early by the release-pace budget */
        statistics::Scalar pacedStops;
    } vtycheStats;

    /**
     * Self-clocked drain (every cycle): the conversion/emission
     * engine fires on its own clock while work is buffered, instead of
     * waiting for the next demand access. The stream walk stays
     * demand-anchored (limitAddr only advances with demand), so this
     * moves emission earlier WITHIN the window; it cannot extend it.
     *
     * insert() needs a demand access's translation context (VA/PA and
     * contextId for cross-page translation requests), which probe
     * callbacks receive but self-scheduled events do not — so notify()
     * latches the most recent demand's Request (a shared_ptr, safe to
     * hold), a PrefetchInfo template, and the cache accessor (same
     * lifetime pattern as DeferredPacket's stored accessor pointer).
     */
    EventFunctionWrapper drainEvent;
    /** Most recent demand request; keeps the translation context alive */
    RequestPtr drainCtxReq;
    /** PrefetchInfo template cloned per emitted target */
    std::unique_ptr<PrefetchInfo> drainCtxPfi;
    /** The hosting cache's accessor (lives as long as the cache) */
    const CacheAccessor *drainCtxCache = nullptr;

    /** Any producer holding buffered or half-emitted conversion work? */
    bool drainWorkPending() const;
    /** Arm the drain event if enabled, unarmed, and work is pending */
    void scheduleDrain();
    /** One self-clocked firing: emit into real queue room, re-arm */
    void drainTick();

    /**
     * Collapse a chain snapshot into the linear form, or return an
     * invalid form if the chain is not affine with a power-of-2 scale.
     * See the header comment for the accepted algebra.
     */
    LinearForm collapse(const VectorChainTable::ChainSnapshot &snap) const;

    /** Apply the slice's extension, then the shift-and-add lane */
    Addr applyForm(const LinearForm &f, uint64_t raw) const;

    /** Scan the producer's dedup window for a target line */
    bool inDedupWindow(const SttEntry &ps, Addr line) const;

    /** The architectural stream: emit lines in (highWater,
     *  addr + size*(D+1)], raise the mark */
    void streamAhead(SttEntry &ps, Addr addr, unsigned size,
                     std::vector<AddrPriority> &addresses);

    /** Register one index line for capture at fill (bounded FIFO) */
    void registerCapture(Addr line_pa, Addr line_va, Addr producer_pc,
                         bool secure, bool from_miss);

    /**
     * Convert one captured index line in a single event: every element
     * through its own shift-and-add lane, line-deduplicated into the
     * producer's output latch. This is the parallel hardware — GDP
     * walks the same payload element-serially instead.
     */
    void convertLine(SttEntry &ps, const CapturedLine &cap);

    /**
     * Release converted targets into free prefetch-queue slots, at
     * least drainFloor per event; called on every observed access,
     * which supplies the translation context.
     */
    void drainEmission(std::vector<AddrPriority> &addresses);

  public:
    VectorTyche2(const VectorTyche2PrefetcherParams &p);
    ~VectorTyche2() = default;

    /** Latch the drain's translation context, then delegate; arms the
     *  self-clocked drain afterwards */
    void notify(const CacheAccessProbeArg &acc,
                const PrefetchInfo &pfi) override;

    void calculatePrefetch(const PrefetchInfo &pfi,
                           std::vector<AddrPriority> &addresses,
                           const CacheAccessor &cache) override;

    /** Observe issuing prefetch packets: register lines that fall in a
     *  configured producer's stream window (range-based capture) */
    PacketPtr getPacket() override;

    void notifyFill(const CacheAccessProbeArg &acc) override;

    void resetLearnedState() override;
};

} // namespace prefetch
} // namespace gem5

#endif //__MEM_CACHE_PREFETCH_VECTOR_TYCHE2_HH__
