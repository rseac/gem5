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
 * Four opt-in conversion/emission knobs (VHybrid's, so the two stay
 * comparable on these axes; all default-off, leaving the classic
 * behavior above unchanged): conversion_lanes bounds the lane array
 * at N elements per event, a wider line resuming at the buffer front
 * next event; drop_on_full drops overflow targets at a full queue
 * instead of the drain_floor displacement; vl_window_dedup rescopes
 * the within-line seen-set to vl_window_bytes-aligned index-stream
 * windows (~one gather chunk); pre_lane_dedup moves the dedup compare
 * before the lanes, onto truncated folded indices, so a hit spends no
 * lane circuit and no lane-budget slot. Full semantics in
 * Prefetcher.py; the mechanics follow mem/cache/prefetch/vhybrid.hh.
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

#ifndef __MEM_CACHE_PREFETCH_VECTOR_TYCHE_HH__
#define __MEM_CACHE_PREFETCH_VECTOR_TYCHE_HH__

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

struct VectorTychePrefetcherParams;

namespace prefetch
{

class VectorTyche : public Queued
{
    /** The CPU-side chain/link table (shared SimObject, same one GDP
     *  uses — the discovery half is identical) */
    VectorChainTable *const tbl;
    /** Optional announcement sideband (ReVeLA's STT); read only when
     *  limitGate is set. nullptr when the gate is unused. */
    RevelaStreamTable *const announceTbl;
    /** Announced-limit gate: clamp the walk and the conversion at the
     *  vsetvl-AVL-announced extent end (no-op where no announcement
     *  covers a line) */
    const bool limitGate;
    /** Stream lookahead in lines (sets the indirect lookahead too) */
    const int streamingDistance;
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
    /** Also latch index lines off the read port on a demand read HIT,
     *  not just from fills (see Prefetcher.py) */
    const bool captureOnReadHit;
    /** Minimum emissions per access event when the queue is full */
    const int drainFloor;
    /** Cross-line dedup window in index lines; 0 disables */
    const unsigned dedupBufferSize;
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
    /** Window span in index-stream bytes (power of two, >= blkSize;
     *  a full chunk always consumes VLEN/8 index bytes) */
    const unsigned vlWindowBytes;
    const unsigned vlWindowShift;
    /** Placement of the dedup compare: true = BEFORE the shift/add
     *  lanes, on truncated folded indices (extension is wire fan-out,
     *  the base's sub-line offset folds in at insertion) — a hit
     *  consumes no lane circuit and no conversion_lanes budget slot.
     *  False = after conversion, on target line addresses. */
    const bool preLaneDedup;
    /** Consumer ways per producer: shift/add lane groups fed by ONE
     *  broadcast slice buffer, one group per linked gather
     *  (multi-way A[B[i]] + C[B[i]]). Must match the chain table's
     *  consumers_per_producer (the config script wires both). */
    const unsigned consumersPerProducer;
    /** DRAM row shift for row-aware issue order; 0 disables */
    const unsigned rowScheduleBits;
    /** Row-schedule scan depth in queue entries, head included;
     *  0 = unbounded (scan the head's whole priority group) */
    const unsigned reorderWindowSize;

    /**
     * DRAM row-aware issue order (row_schedule_bits > 0).
     *
     * The prefetch queue issues FIFO within a priority level, so two
     * queued targets in the same DRAM row can be separated by enough
     * intervening issues that the row closes between them, and the
     * second pays a full tRP + tRCD instead of riding the open row.
     * Before delegating to Queued::getPacket, promote the oldest
     * already-due, equal-priority entry whose row matches the last
     * issued prefetch to the head, so row-mates leave back-to-back.
     *
     * Equal priority keeps DeferredPacket's ordering (by priority,
     * see queued.hh) intact, and requiring the head to be due keeps
     * nextPrefetchReadyTime honest. Reordering is confined to entries
     * that could all have issued this cycle anyway, so no prefetch is
     * delayed past its ready time — only the order among ready ones
     * changes.
     *
     * The scan is bounded to the first reorderWindowSize queue
     * entries (0 = unbounded), modeling a fixed-depth comparator
     * window at the queue head rather than a CAM over the whole
     * queue. Entries inside the window that fail the due/priority
     * checks still occupy window slots.
     */
    void rowSchedule();

    /** Row of the last issued prefetch (valid when lastRowValid) */
    Addr lastRow = 0;
    bool lastRowValid = false;

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
        /** Bytes below the announced extent limit (0 = whole line;
         *  only ever nonzero under limit_gate) */
        unsigned validBytes = 0;
        /** Conversion cursor: elements already through the lane
         *  array (a line wider than conversion_lanes resumes here) */
        unsigned nextElem = 0;
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
        /** One converted target with the consumer group that
         *  produced it (group routes the per-way dedup bookkeeping
         *  at emission; always 0 at consumers_per_producer = 1) */
        struct TargetEntry
        {
            Addr addr;
            uint8_t group;
        };
        /** Distinct target lines, element-precise, element-major
         *  interleaved across groups (A[B[0]], C[B[0]], A[B[1]]...) */
        std::vector<TargetEntry> targets;
        /** Next target to emit (the emission cursor) */
        unsigned next = 0;
        /** This chunk finishes its source line: emitting it fully
         *  closes the line's dedup-window entries */
        bool lastChunk = false;
    };

    /**
     * One consumer way of a producer: the collapsed form of one
     * linked gather plus its dedup bookkeeping. The raw slice buffer
     * is shared across ways (it stages index data, which is
     * base-agnostic); everything downstream of the broadcast — the
     * form, the seen-set, the cross-line window — is per-way because
     * the target spaces are disjoint.
     */
    struct ConsumerGroup
    {
        /** The adopted linear form */
        LinearForm form;
        bool configured = false;
        /** The within-window seen-set. Scope and key space depend on
         *  the mode: per captured source line (default) or per
         *  vl-window (vl_window_dedup); target line addresses
         *  (default) or truncated folded index keys
         *  (pre_lane_dedup). Carries dedup across conversion_lanes
         *  chunks in every mode. */
        std::vector<Addr> lineSeen;
        /** Target lines EMITTED from the current source line; closes
         *  into one dedup-window entry with the line's last chunk
         *  (only maintained when dedup_buffer_size > 0) */
        std::vector<Addr> pendingEmitted;
        /** Emitted target lines of the last dedup_buffer_size
         *  processed index lines, oldest first (the cross-line CAM;
         *  empty when dedup_buffer_size = 0) */
        std::deque<std::vector<Addr>> dedupWindow;
    };

    /** Per-producer runtime state (stream walk + consumer ways) */
    struct SttEntry
    {
        Addr lastAddr = 0;
        bool valid = false;
        /** Stream high-water mark (exclusive end of issued window) */
        Addr limitAddr = 0;
        /** Consumer ways, indexed by chain-table consumer slot (up
         *  to consumers_per_producer; one entry at the default) */
        std::vector<ConsumerGroup> groups;
        /** Slice width latched with the forms (producer EEW/8 — a
         *  property of the index array, shared by every way) */
        unsigned elemBytes = 0;
        /** Captured raw index lines awaiting conversion (shared:
         *  the broadcast source for every way's lane group) */
        std::deque<CapturedLine> sliceBuffer;
        /** The lane array's output latch (valid when latchValid) */
        ConvertedLine latch;
        bool latchValid = false;
        /** Current vl-window bucket (vl_window_dedup): keyed on the
         *  index-stream VA, so it is shared across ways; a bucket
         *  crossing clears every way's seen-set */
        Addr windowKey = MaxAddr;
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
        /** Announced-extent byte clamp captured at registration
         *  (0 = whole line) */
        unsigned validBytes;
    };
    std::list<IrtEntry> indexRoutingTable;

    struct VTycheStats : public statistics::Group
    {
        VTycheStats(statistics::Group *parent);
        /** Chunk events at producer PCs */
        statistics::Scalar chunksObserved;
        /** Index-array stream candidates emitted */
        statistics::Scalar streamCandidates;
        /** Walk lines suppressed at the announced extent end
         *  (limit_gate) */
        statistics::Scalar streamLimitClamped;
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
        /** Resident index lines latched off the read port on a demand
         *  read hit (capture_on_read_hit) */
        statistics::Scalar hitsCaptured;
        /** Fills dropped: slice buffer full */
        statistics::Scalar bufferBusyDrops;
        /** Fills dropped: form stale (table cleared) */
        statistics::Scalar staleConfigs;
        /** Elements pushed through a shift-and-add lane */
        statistics::Scalar elementsConverted;
        /** Elements not converted: beyond the announced extent
         *  (limit_gate) */
        statistics::Scalar elementsBeyondLimit;
        /** Elements dropped by the pre-lane truncated-index compare
         *  (consumed no shift/add lane and no lane-budget slot) */
        statistics::Scalar elementsPreDeduped;
        /** Conversion events cut short by the lane budget (the line
         *  resumed on a later event) */
        statistics::Scalar conversionWidthLimited;
        /** Elements whose slot was compacted out: every configured
         *  way's pre-lane CAM hit, so no lane fired and no
         *  conversion_lanes slot was spent. At one way this counts
         *  the same events as elementsPreDeduped; with several ways
         *  it isolates the all-ways-aligned duplicates (the
         *  compaction the any-miss slot rule banks on). */
        statistics::Scalar slotsCompacted;
        /** Indirect target candidates generated */
        statistics::Scalar targetsGenerated;
        /** Targets dropped by the canonical-address filter */
        statistics::Scalar targetsFiltered;
        /** Targets dropped as duplicate lines within one batch */
        statistics::Scalar targetsDeduplicated;
        /** Targets dropped: line emitted within the dedup window */
        statistics::Scalar targetsCrossDeduplicated;
        /** Targets dropped at a full queue (drop_on_full) instead of
         *  displacing queued entries */
        statistics::Scalar targetsDroppedFull;
        /** Batches flushed: walk cursor passed their index line */
        statistics::Scalar stalenessAborts;
        /** Emission events paused by a full queue (the stall firing) */
        statistics::Scalar emissionDeferred;
        /** Events where the array's one-line conversion slot ended
         *  with further lines still buffered */
        statistics::Scalar emissionLineLimited;
        /** Self-clocked drain firings that ran the emission engine */
        statistics::Scalar selfDrainTicks;
        /** Issues where a row-mate was promoted to the queue head */
        statistics::Scalar rowPromotions;
        /** Issues where the queue held no due row-mate to promote */
        statistics::Scalar rowScheduleMisses;
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

    /**
     * The access packet of the notify() currently in flight, or null.
     * calculatePrefetch only receives the PrefetchInfo, whose captured
     * data is not byte-addressable, so capture_on_read_hit reads the
     * resident line's payload off this packet instead — the same
     * latch-then-delegate pattern notify() already uses for the drain
     * context above. Only ever read inside the synchronous
     * Queued::notify call that set it; drainTick never touches it.
     */
    const Packet *hitPkt = nullptr;

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

    /** The form's index extension (sign/zero, extBits) alone */
    uint64_t extendRaw(const LinearForm &f, uint64_t raw) const;

    /** Apply the slice's extension, then the shift-and-add lane */
    Addr applyForm(const LinearForm &f, uint64_t raw) const;

    /** Pre-lane dedup key: same-target-line iff equal. Extend the
     *  raw index, shift (wire select), fold the base's sub-line
     *  offset (6-bit constant add), truncate to line granularity —
     *  no full-width base add, so it runs before lane allocation. */
    Addr preLaneKey(const LinearForm &f, uint64_t raw) const;

    /** Scan one way's dedup window for a target line */
    bool inDedupWindow(const ConsumerGroup &cg, Addr line) const;

    /** Any way holding an adopted form (the producer owns a
     *  pipeline)? */
    bool anyConfigured(const SttEntry &ps) const;

    /** Any way whose form survives the current table generation?
     *  (!anyFresh == the old whole-producer staleness condition) */
    bool anyFresh(const SttEntry &ps) const;

    /** The architectural stream: emit lines in (highWater,
     *  addr + size*(D+1)], raise the mark. announced_limit != 0 caps
     *  the walk at the announced extent end (limit_gate). */
    void streamAhead(SttEntry &ps, Addr addr, unsigned size,
                     Addr announced_limit,
                     std::vector<AddrPriority> &addresses);

    /** Register one index line for capture at fill (bounded FIFO);
     *  valid_bytes != 0 clamps its conversion (limit_gate) */
    void registerCapture(Addr line_pa, Addr line_va, Addr producer_pc,
                         bool secure, bool from_miss,
                         unsigned valid_bytes);

    /** Drop a registered line, so a capture taken elsewhere (the read
     *  port) is not latched a second time by a later fill */
    void dropRegistration(Addr line_pa, bool secure);

    /** capture_on_read_hit: latch the resident index lines this demand
     *  read hit on straight into the producer's slice buffer, reading
     *  the payload off hitPkt (no IRT round trip — a hit holds the
     *  bytes, the VA and the PA at once) */
    void captureFromReadHit(SttEntry &ps, const PrefetchInfo &pfi,
                            Addr pc, Addr addr, unsigned size,
                            bool is_secure);

    /** Smallest announced extent end above line_va for this producer
     *  PC (0 = none / gate off; same aliasing rule as vhybrid.cc) */
    Addr announcedLimit(Addr pc, Addr line_va) const;

    /** Byte clamp for an index line under the announced limit
     *  (0 = whole line) */
    unsigned gateBytes(Addr pc, Addr line_va) const;

    /**
     * Convert the next up-to-conversion_lanes elements of the line
     * into the producer's output latch (0 = the whole line in one
     * event, every element through its own shift-and-add lane — the
     * classic parallel array; GDP walks the same payload
     * element-serially instead). Returns true when the line is
     * exhausted.
     */
    bool convertChunk(SttEntry &ps, CapturedLine &cap);

    /**
     * Release converted targets into free prefetch-queue slots, at
     * least drainFloor per event; called on every observed access,
     * which supplies the translation context.
     */
    void drainEmission(std::vector<AddrPriority> &addresses);

  public:
    VectorTyche(const VectorTychePrefetcherParams &p);
    ~VectorTyche() = default;

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

#endif //__MEM_CACHE_PREFETCH_VECTOR_TYCHE_HH__
