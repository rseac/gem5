/**
 * VHybrid — announcement-driven gather prefetcher: ReVeLA's stream
 * engine driving Viper's capture/convert half.
 *
 * The composition (both halves reused structurally unchanged, so each
 * is directly comparable to its standalone parent):
 *
 *  - STREAM half = ReVeLA (mem/cache/prefetch/revela.hh): streams are
 *    ANNOUNCED by the vsetvl AVL and tracked in the CPU-side
 *    RevelaStreamTable (cpu/revela_table.hh) — no training, exact
 *    extents, the Aggressivity Table splitting the in-flight budget,
 *    min-distance fairness, and the every-cycle self-clocked drain.
 *    This replaces Viper's demand-anchored per-producer stream walk:
 *    lookahead is licensed by the announced extent (up to
 *    max_prefetch_distance lines) instead of demand + sd.
 *
 *  - INDIRECT half = Viper (mem/cache/prefetch/viper.hh): the
 *    VectorChainTable (cpu/vector_chain_table.hh) names producer
 *    unit-stride loads, links each to its gather in one iteration and
 *    snoops the gather's base; the chain collapses at adoption into
 *    target = base + (extend(idx) << shift); departing stream
 *    prefetches that fall inside a producer stream register their PA
 *    in the Index Routing Table; fills latch raw payloads into per-
 *    producer slice buffers; the drain converts ONE line per event
 *    through the lane array — up to conversion_lanes elements per
 *    event (0 = the whole line at once), a wider line resuming next
 *    event — and releases deduped targets into queue room.
 *
 *  - The JOIN: an STT entry carries the PC of the last access that
 *    updated it (SttEntry::lastPc, stamped by the LSQ report path).
 *    A stream whose lastPc the chain table knows as a linked producer
 *    is an INDEX stream: its emitted lines are registered for capture
 *    at getPacket (the one point holding the VA and translated PA
 *    together) and its physical pages are published for stream-aware
 *    replacement. Producer status is re-evaluated from the chain
 *    table on use, never latched in the STT, so late-forming links
 *    and wholesale chain-table clears self-heal.
 *
 * Per-tick emission order: targets first (their gather demand is
 * closer than any stream frontier), then the stream round with the
 * remaining room; both count against one combined
 * pfq + pfqMissingTranslation budget.
 *
 * Deliberately NOT included: announcement stitching. Streams
 * announced per-row (nested-loop CSR spmv) keep their tiny extents
 * and the RVL<=GVL allocation skip — that shape is out of coverage
 * by design; the JDS spmv variant is the sparse evaluation vehicle.
 */

#ifndef __MEM_CACHE_PREFETCH_VHYBRID_HH__
#define __MEM_CACHE_PREFETCH_VHYBRID_HH__

#include <deque>
#include <list>
#include <memory>
#include <unordered_map>
#include <vector>

#include "base/statistics.hh"
#include "cpu/revela_table.hh"
#include "cpu/vector_chain_table.hh"
#include "mem/cache/prefetch/queued.hh"

namespace gem5
{

struct VHybridPrefetcherParams;

namespace prefetch
{

class VHybrid : public Queued
{
    /** CPU-side stream table (shared with the CPU's revela_table) */
    RevelaStreamTable *const streamTbl;
    /** CPU-side chain/link table (shared with vector_chain_table) */
    VectorChainTable *const chainTbl;

    /** Aggressivity ceiling in lines (halved at 2/4/8 live streams) */
    const unsigned maxPrefetchDistance;
    /** Lines per min-distance stream per trigger evaluation */
    const unsigned degree;
    /** Emission floor in lines ahead of current@ (revela.hh has the
     *  full story). Skipped index lines still convert through the
     *  demand-miss capture registration — late, like the skipped
     *  region itself. 0 = current behavior. */
    const unsigned initialDistance;
    /** Ablation: streams only, never capture/convert (= ReVeLA) */
    const bool streamOnly;
    /** Captured raw index lines per producer slice buffer */
    const unsigned sliceBufferEntries;
    /** Concurrently configured producer pipelines */
    const unsigned pipelines;
    /** Index Routing Table capacity */
    const unsigned irtEntries;
    /** Minimum target emissions per demand event on a full queue */
    const int drainFloor;
    /** Cross-line dedup window in index lines; 0 disables */
    const unsigned dedupBufferSize;
    /** Clamp conversion at the announced stream extent (vsetvl AVL):
     *  bytes of a captured line beyond limit@ are not index data */
    const bool limitAwareSlicing;
    /** Shift-and-add circuits: index elements converted per drain
     *  event (0 = unbounded, the whole captured line at once) */
    const unsigned conversionLanes;
    /** Queue-full disposition for converted targets: true = drop the
     *  overflow target (queue keeps its entries), false = the
     *  drain_floor displacement license (targets push into the full
     *  queue, evicting its oldest entries) */
    const bool dropOnFull;
    /** vl-aware dedup scope: the within-window seen-set clears at
     *  vl_window_bytes-aligned index-stream boundaries (~one gather
     *  chunk) instead of per captured line. Within one chunk the LSQ
     *  coalesces same-line elements onto one MSHR regardless of cache
     *  state, so this window suppresses exactly the guaranteed-safe
     *  duplicates and never bets on residency. */
    const bool vlWindowDedup;
    /** Window span in index-stream BYTES: a full chunk always
     *  consumes VLEN/8 index bytes regardless of EEW (vl x elemBytes
     *  = VLEN/8). Power of two; boundaries are VA-aligned buckets —
     *  exact instruction boundaries when the stream starts
     *  chunk-aligned, a constant phase shift otherwise. */
    const unsigned vlWindowBytes;
    const unsigned vlWindowShift;
    /** Placement of the dedup compare: true = BEFORE the shift/add
     *  lanes, on truncated folded indices (extension is wire fan-out,
     *  the base's sub-line offset folds in at insertion) — a hit
     *  consumes no lane circuit and no conversion_lanes budget slot.
     *  False = after conversion, on target line addresses. */
    const bool preLaneDedup;

    /** The collapsed chain (Viper's LinearForm, unchanged) */
    struct LinearForm
    {
        bool valid = false;
        Addr base = 0;
        unsigned shift = 0;
        unsigned extBits = 0;
        bool extSigned = false;
        uint64_t gen = 0;
    };

    /** One captured index line, latched raw at the fill */
    struct CapturedLine
    {
        Addr lineVaddr = 0;
        std::vector<uint8_t> data;
        /** Bytes inside the announced extent (rest is not index data;
         *  0 = unknown, convert the whole line) */
        unsigned validBytes = 0;
        /** Conversion cursor: elements already through the lane
         *  array (a line wider than conversion_lanes resumes here) */
        unsigned nextElem = 0;
    };

    /** The lane array's output latch (one converted chunk) */
    struct ConvertedLine
    {
        Addr lineVaddr = 0;
        std::vector<Addr> targets;
        unsigned next = 0;
        /** This chunk finishes its source line: emitting it fully
         *  closes the line's dedup-window entry */
        bool lastChunk = false;
    };

    /**
     * Per-producer pipeline state, keyed by producer PC. This is
     * Viper's SttEntry minus the stream walk (lastAddr survives only
     * to throttle adoption to chunk events; the walk itself lives in
     * the RevelaStreamTable now).
     */
    struct PipeEntry
    {
        bool valid = false;
        Addr lastAddr = 0;
        LinearForm form;
        bool configured = false;
        unsigned elemBytes = 0;
        std::deque<CapturedLine> sliceBuffer;
        ConvertedLine latch;
        bool latchValid = false;
        std::deque<std::vector<Addr>> dedupWindow;
        /** The within-window seen-set. Scope and key space depend on
         *  the mode: per captured source line (default) or per
         *  vl-window (vl_window_dedup); target line addresses
         *  (default) or truncated folded index keys
         *  (pre_lane_dedup). Carries dedup across conversion_lanes
         *  chunks in every mode. */
        std::vector<Addr> lineSeen;
        /** Current vl-window bucket (vl_window_dedup): the seen-set
         *  clears when an element's VA leaves this bucket */
        Addr windowKey = MaxAddr;
        /** Target lines EMITTED from the current source line; closes
         *  into one dedup-window entry with the line's last chunk */
        std::vector<Addr> pendingEmitted;
    };
    std::unordered_map<Addr, PipeEntry> pipeTable;
    unsigned configuredCount = 0;

    /** Index line registered for capture at fill */
    struct IrtEntry
    {
        Addr linePaddr;
        Addr lineVaddr;
        Addr producerPC;
        bool secure;
        /** Bytes inside the announcing stream's extent at
         *  registration (0 = unknown) */
        unsigned validBytes;
    };
    std::list<IrtEntry> indexRoutingTable;

    struct VHybridStats : public statistics::Group
    {
        VHybridStats(statistics::Group *parent);
        // Stream half (ReVeLA's counters)
        statistics::Scalar selfDrainTicks;
        statistics::Scalar linesEmitted;
        statistics::Scalar drainNoRoom;
        statistics::Scalar aggressivityThrottled;
        statistics::Scalar linesSkippedFloor;
        // Indirect half (Viper's counters)
        statistics::Scalar producerChunks;
        statistics::Scalar formsAdopted;
        statistics::Scalar chainNotReady;
        statistics::Scalar chainsRejected;
        statistics::Scalar pipelinesSaturated;
        statistics::Scalar capturesRegistered;
        statistics::Scalar capturesRegisteredMiss;
        statistics::Scalar fillsCaptured;
        statistics::Scalar bufferBusyDrops;
        statistics::Scalar staleConfigs;
        statistics::Scalar elementsConverted;
        statistics::Scalar elementsBeyondLimit;
        statistics::Scalar conversionWidthLimited;
        statistics::Scalar elementsPreDeduped;
        statistics::Scalar targetsGenerated;
        statistics::Scalar targetsFiltered;
        statistics::Scalar targetsDeduplicated;
        statistics::Scalar targetsCrossDeduplicated;
        statistics::Scalar targetsDroppedFull;
        statistics::Scalar stalenessAborts;
        statistics::Scalar emissionDeferred;
        statistics::Scalar emissionLineLimited;
    } vhybridStats;

    /** Self-clocked drain (every cycle, both halves; same latched
     *  translation-context pattern as revela.hh/viper.hh) */
    EventFunctionWrapper drainEvent;
    RequestPtr drainCtxReq;
    std::unique_ptr<PrefetchInfo> drainCtxPfi;
    const CacheAccessor *drainCtxCache = nullptr;

    // ---- stream half (ReVeLA logic against the shared STT) ----
    unsigned aggressivityLimit() const;
    unsigned distanceLines(const RevelaStreamTable::SttEntry &e) const;
    /** Jump a stream's frontier to the initial-distance floor
     *  (no-op at initial_distance=0) */
    void applyInitialDistance(RevelaStreamTable::SttEntry &e);
    bool streamWorkPending() const;
    /** One stream emission round; room accounts for targets already
     *  staged in `addresses` this event */
    void emitStreamRound(std::vector<AddrPriority> &addresses);

    // ---- indirect half (Viper logic against pipeTable) ----
    LinearForm collapse(const VectorChainTable::ChainSnapshot &s) const;
    /** The form's index extension (sign/zero, extBits) alone */
    uint64_t extendRaw(const LinearForm &f, uint64_t raw) const;
    Addr applyForm(const LinearForm &f, uint64_t raw) const;
    /** Pre-lane dedup key: same-target-line iff equal. Extend the
     *  raw index, shift (wire select), fold the base's sub-line
     *  offset (6-bit constant add), truncate to line granularity —
     *  no full-width base add, so it runs before lane allocation. */
    Addr preLaneKey(const LinearForm &f, uint64_t raw) const;
    bool inDedupWindow(const PipeEntry &pe, Addr line) const;
    void registerCapture(Addr line_pa, Addr line_va, Addr producer_pc,
                         bool secure, bool from_miss,
                         unsigned valid_bytes);
    /** Bytes of the line at line_va inside an extent ending at
     *  limit_addr (blkSize when unclamped or the feature is off) */
    unsigned clampBytes(Addr limit_addr, Addr line_va) const;
    /** Announced limit@ covering line_va for this producer's live
     *  stream(s): the smallest limitAddr above line_va among STT
     *  entries stamped with this PC (0 = no live announcement — the
     *  demand-miss capture path has no STT entry in hand) */
    Addr announcedLimit(Addr pc, Addr line_va) const;
    /** Convert the next up-to-conversion_lanes elements of the line
     *  into the latch; returns true when the line is exhausted */
    bool convertChunk(PipeEntry &pe, CapturedLine &cap);
    void drainEmission(std::vector<AddrPriority> &addresses);
    bool conversionWorkPending() const;
    /** Demand cursor of the producer's live stream: the STT entry
     *  whose lastPc matches (0 = no live stream, no staleness abort) */
    Addr streamCursor(Addr pc) const;

    // ---- drain plumbing ----
    void scheduleDrain();
    void drainTick();

  public:
    VHybrid(const VHybridPrefetcherParams &p);
    ~VHybrid() = default;

    void notify(const CacheAccessProbeArg &acc,
                const PrefetchInfo &pfi) override;
    void calculatePrefetch(const PrefetchInfo &pfi,
                           std::vector<AddrPriority> &addresses,
                           const CacheAccessor &cache) override;
    /** Peek the departing prefetch: index-stream lines register for
     *  capture and publish their page (range capture, Viper's) */
    PacketPtr getPacket() override;
    void notifyFill(const CacheAccessProbeArg &acc) override;
    void resetLearnedState() override;
};

} // namespace prefetch
} // namespace gem5

#endif //__MEM_CACHE_PREFETCH_VHYBRID_HH__
