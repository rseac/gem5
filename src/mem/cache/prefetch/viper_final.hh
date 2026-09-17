/**
 * ViperFinal — VIPER-FINAL, the shipping subset of the VIPER A[B[i]]
 * gather prefetcher (cache side).
 *
 * Same engine as mem/cache/prefetch/viper.hh — read that file for the
 * design rationale, the accepted algebra, and the capture/convert/emit
 * structure. This variant separates the mechanisms that earned their
 * place from the ones built to be measured: each item below is DELETED
 * from the source, not defaulted off, so nothing here is a
 * configuration of something larger.
 *
 * Removed subsystems:
 *   1. the SCALAR LEVEL CHAIN (scalar_chain_enable) and all of its
 *      apparatus — the TycheChainTable sideband, the IP-stride head
 *      trigger, compiled scalar levels in the stream table, the Form B
 *      terminal hop, resident capture of chain lines, the lineage TTL,
 *      and the demand_span_chained span gate it armed. This drops the
 *      file's only dependence on cpu/tyche_table.hh;
 *   2. RESIDENCY-INTERVAL registers (residency_intervals);
 *   3. the SET-PRESSURE gate (set_pressure_threshold) and its per-set
 *      counters and prefetch-origin line set;
 *   4. STREAM-PAGE REGISTRY publication (registerStreamPage), which
 *      fed StreamDemoteLRURP;
 *   5. DRAM ROW-AWARE ISSUE REORDERING (row_schedule_bits).
 *
 * Removed policies and mechanisms:
 *   6. SPAN-COMPLETION emission (demand_span_only). The run-ahead walk
 *      is the only emission policy;
 *   7. the access-size-relative STREAMING_DISTANCE walk (and
 *      stream_start_at_distance). prefetch_distance is the only
 *      lookahead knob and is mandatory; since 2026-09-12 it counts
 *      CACHE LINES (it was whole-VLEN chunks with a vlen param), so
 *      the frontier means the same number of bytes at every VLEN and
 *      every vl with no VLEN plumbing into the prefetcher;
 *   8. VL-WINDOW dedup (vl_window_dedup). The seen-set has one scope
 *      by default: it clears at each captured line. Since 2026-09-06
 *      seen_set_entries > 0 instead makes it a persistent FIFO CAM
 *      of that many keys that never clears between lines;
 *   9. the announced-extent clamp on SLICING (validBytes/gateBytes).
 *      limit_gate survives but bounds only where the WALK may go — a
 *      captured line is always converted whole, because it was
 *      captured from inside a producer's own window or from a demand
 *      miss at a producer PC, so its bytes are index data by
 *      construction;
 *  10. STRIDED (vlse) producers. Only unit-stride index loads are
 *      tracked: a strided producer issues one access per element,
 *      which needs a span model the walk does not have. The chain
 *      table still REPORTS strided heads (ProducerInfo::stride), so
 *      they are rejected explicitly in calculatePrefetch — deleting
 *      the span model alone left them mis-modelled as unit-stride,
 *      which is worse than either tracking or ignoring them (measured
 *      on sssp: 4.3x the chunk events, +17% prefetches).
 *
 * Redundant state removed (2026-09-02). Each of these was a field that
 * encoded something another field already determined, or a second
 * copy of a mechanism that existed once elsewhere:
 *  11. the OP-LIST adoption path (pipelineConfig + an engine-side
 *      collapse()). The chain table's backward walk folds every chain
 *      and pushes the result into the IPT here (folded_forms, now
 *      REQUIRED); the engine held a second fold of the same chain and
 *      a second copy of the form in each consumer way;
 *  12. per-way "configured" bits and the producer-slot counter. A way
 *      is live iff its IPT record holds a valid form; a producer holds
 *      a slot iff it holds an IPT set. producer_slots therefore SIZES
 *      the IPT (ipt_sets is gone), and the chain table's `linked` bit
 *      is no longer read at all;
 *  13. the STT "valid" bit (a row exists only once it has seen a
 *      chunk, so its presence is the bit) and the output latch's
 *      "valid" bit (the latch is live iff its cursor is short of its
 *      count);
 *  14. the polled "table was cleared" flag beside the pushed
 *      invalidates. Every invalidate — per gather, capacity clear, or
 *      unresolvable head — now arrives on the FormSink, and edits the
 *      way records directly;
 *  15. the duplicated post-lane dedup path. The seen-set always holds
 *      folded index keys, which name target lines exactly (line =
 *      key + base>>6 is a bijection at fixed base), so one compare
 *      serves both placements; pre_lane_dedup now decides only
 *      whether a hit spends a conversion_lanes slot;
 *  16. (2026-09-03) the STT's slice-width field. The producer's EEW
 *      is a property of its index array, written once on the DCT
 *      head row and never refreshed, and it is read only by the lane
 *      at conversion — which runs only for a producer holding an IPT
 *      set. It now rides on the form push and lives once per IPT
 *      set, beside the ways the lane reads it with. The STT copy was
 *      rewritten to the same value at every chunk event and was dead
 *      on every stream-only row.
 *
 * Everything else is preserved verbatim: architectural discovery via
 * the shared VectorChainTable, range-based fill capture, per-producer
 * index line queues, whole-line lane-array conversion, cross-line /
 * pre-lane / eviction-clocked dedup, the retired-IRT tail, capture
 * admission backpressure, the random tail-drop throttle,
 * batch-confidence feedback, multi-way consumers, and the
 * stall-on-full demand-clocked drain.
 */
#ifndef __MEM_CACHE_PREFETCH_VIPER_FINAL_HH__
#define __MEM_CACHE_PREFETCH_VIPER_FINAL_HH__

#include <deque>
#include <list>
#include <memory>
#include <random>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "base/statistics.hh"
#include "cpu/revela_table.hh"
#include "cpu/vector_chain_table.hh"
#include "mem/cache/prefetch/queued.hh"

namespace gem5
{

struct ViperFinalPrefetcherParams;

namespace prefetch
{

class ViperFinal : public Queued, public VectorChainTable::FormSink
{
  public:
    // ---- VectorChainTable::FormSink ----
    // The IPT lives here, beside the lane array that reads it, and
    // the discovery half writes it across once for each chain.
    void installForm(Addr producer_pc, int consumer_dct_ptr,
                     unsigned elem_bytes, unsigned nfields,
                     const VectorChainTable::FoldedForm &f) override;
    void invalidateForm(Addr producer_pc, int consumer_dct_ptr) override;
    void invalidateAllForms() override;

  private:
    /** The collapsed chain the lane array executes:
     *  target = base + ((extend(elem) >> rshift) << shift). One
     *  shifter and one adder per lane; the whole "pipeline
     *  configuration" is these operands. Folded by the chain table's
     *  backward walk and pushed here; this file never derives one. */
    using Form = VectorChainTable::FoldedForm;

    /** One way of an IPT set: the folded chain of a single consumer.
     *  The way is LIVE iff form.valid — a push always arrives valid
     *  and an invalidate clears it, so there is no second occupancy
     *  bit. consumerDctPtr is the routing key an invalidate names;
     *  on a cleared way it is stale and don't-care, like a free
     *  set's tag. */
    struct IptWay
    {
        int consumerDctPtr = -1;
        Form form;
    };
    /** One IPT set: every folded form of one producer. Found by
     *  producer PC, so the table sizes to the producers that can hold
     *  a pipeline (producer_slots). A set is in use while any way is
     *  live; a free set keeps a stale tag that the occupancy test
     *  stops from matching. */
    struct IptSet
    {
        Addr pcTag = 0;
        /** Slice width of the producer's index array (EEW/8), shared
         *  by every way; written with each form push, from the DCT
         *  head row. Four legal values {1,2,4,8}, so a 2-bit log2
         *  code in hardware — no "unset" state, because the set's
         *  occupancy is already the ways' valid bits. */
        uint8_t elemBytes = 0;
        /** Segment (vlseg) producer field count nf, 0/1 = unit-stride
         *  (every way reads every slot). Per producer like elemBytes:
         *  it is the period of the field slot masks, and each way's
         *  mask is the line's field-0 base mask rotated by the way's
         *  FoldedForm::field within that period. 3 bits. */
        uint8_t nfields = 0;
        std::vector<IptWay> ways;
    };
    std::vector<IptSet> ipt;
    static bool iptOccupied(const IptSet &s);
    int iptFind(Addr pc) const;
    int iptAlloc(Addr pc);
    /** Does this producer hold an IPT set (= a producer slot)? */
    bool producerLive(Addr pc) const { return iptFind(pc) >= 0; }
    /** The live form of one consumer way, or nullptr. This is the
     *  only liveness test a way has. */
    const Form *wayForm(Addr pc, unsigned way) const;

    /** The CPU-side chain/link table (shared SimObject, same one GDP
     *  uses — the discovery half is identical) */
    VectorChainTable *const tbl;
    /** Optional announcement sideband (ReVeLA's own stream table,
     *  unrelated to the STT below); read only when
     *  limitGate is set. nullptr when the gate is unused. */
    RevelaStreamTable *const announceTbl;
    /** Announced-limit gate: clamp the WALK at the vsetvl-AVL
     *  announced extent end; an access with no covering announcement
     *  has its entire walk suppressed (streamAhead). It does NOT
     *  clamp conversion — every captured line is sliced whole. */
    const bool limitGate;
    /** Frontier distance in CACHE LINES: the walk's window sits
     *  this many lines (prefetchDistance * blkSize bytes) ahead of
     *  the demand cursor and is as wide as the demand advance (see
     *  Prefetcher.py). The ONLY lookahead knob — VIPER-FINAL has no
     *  access-size-relative streaming_distance walk and, since
     *  2026-09-12, no VLEN-relative chunk unit either. */
    const unsigned prefetchDistance;
    /** Ablation: stream the index arrays but never capture/convert.
     *  Implemented by refusing every pushed form, so the IPT stays
     *  empty and no producer is ever live. */
    const bool streamOnly;
    /** Captured raw index lines each producer slot's index line queue
     *  holds (GDP's index_line_queue_entries: staging is identical) */
    const unsigned indexLineQueueEntries;
    /** Concurrently configured producers: the number of IPT sets. A
     *  producer whose push finds no set free streams without
     *  converting (see Prefetcher.py) */
    const unsigned producerSlots;
    /** Index Routing Table capacity (registered awaiting-fill lines) */
    const unsigned irtEntries;
    /** Also latch index lines off the read port on a demand read HIT,
     *  not just from fills (see Prefetcher.py) */
    const bool captureOnReadHit;
    /** Retired-IRT tail depth: converted index lines remembered after
     *  they leave the pending IRT (0 disables the tail) */
    const unsigned retiredTailEntries;
    /** Read-hit capture of a line this producer already converted:
     *  true = capture anyway (pre-tail behaviour), false = suppress */
    const bool captureReadHitOwnProducer;
    /** Capture admission backpressure: skip new captures while the
     *  prefetch queue backlog is at or above this (0 disables) */
    const unsigned captureQueueGate;
    /** Batch-granular residency feedback: resident-drops from one
     *  batch before its remaining targets are flushed (0 disables) */
    const unsigned dropBatchConfidence;
    /** Fraction of a confident batch's remaining targets discarded
     *  (1.0 = the whole remainder, the original abort) */
    const double dropBatchFraction;
    /** Minimum emissions per access event when the queue is full */
    const int drainFloor;
    /** Cross-line dedup window in index lines; 0 disables */
    const unsigned dedupBufferSize;
    /** Eviction-clocked dedup table capacity in target lines;
     *  0 disables (see Prefetcher.py) */
    const unsigned evictDedupEntries;
    /** Shift-and-add circuits: index elements converted per drain
     *  event (0 = unbounded, the whole captured line at once) */
    const unsigned conversionLanes;
    /** Queue-full disposition for converted targets: true = drop the
     *  overflow target (queue keeps its entries), false = the
     *  drain_floor displacement license (targets push into the full
     *  queue, evicting its oldest entries) */
    const bool dropOnFull;
    /** Track strided (vlse) index producers (span chunking + slot
     *  masks); false rejects them as before 2026-09-12. */
    const bool stridedProducers;
    /** Accept segment (vlseg) index producers: their captured lines
     *  interleave nf fields and each consumer way converts only its
     *  field's slots. false rejects them like strided_producers=false
     *  rejects vlse (ablation / bit-neutrality check). */
    const bool segmentProducers;
    /** Placement of the seen-set compare relative to the lane array.
     *  The compare itself is the same either way (folded index keys,
     *  see preLaneKey); this decides only whether a hit spends a
     *  conversion_lanes slot: true = the CAM sits before the lanes
     *  and a hit clock-gates the lane (no slot), false = the lane
     *  fires first and the hit is dropped after it (a slot). */
    const bool preLaneDedup;
    /** Seen-set scope. 0 = per source line: each way's CAM clears
     *  when a captured line's first chunk converts, so it holds at
     *  most blkSize/EEW keys and needs no replacement. N > 0 = a
     *  persistent FIFO CAM of N keys per way that never clears
     *  between lines; the oldest key is replaced when it is full
     *  (seenSetReplaced). It still clears when its keys can no
     *  longer name target lines: a form re-push with different
     *  operands (clearWaySeenSet), a producer flush, or a backward
     *  demand jump (with the dedup window). */
    const unsigned seenSetEntries;
    /** Ablation: false = no seen-set at all. Every element fires its
     *  lane and every target reaches the latch, so intra-line
     *  duplicates are left to the emission-side structures (the
     *  dedup window and the eviction-clocked table). */
    const bool seenSetDedup;
    /** Consumer ways per producer: shift/add lane groups fed by ONE
     *  broadcast index line queue, one group per linked gather
     *  (multi-way A[B[i]] + C[B[i]]). Must match the chain table's
     *  consumers_per_producer (the config script wires both). */
    const unsigned consumersPerProducer;
    /** Random tail-drop throttle: epoch length in observed demand
     *  accesses (0 disables; see Prefetcher.py) */
    const unsigned randomDropInterval;
    /** Candidate drop scales the epoch clock draws from uniformly */
    const std::vector<double> randomDropRates;
    /** Memoryless epochs: redraw with probability 1/interval per
     *  access instead of the fixed counter (see Prefetcher.py) */
    const bool randomDropGeometric;
    /** Epoch-length floor for geometric mode: shifted geometric,
     *  mean = min + interval (see Prefetcher.py) */
    const unsigned randomDropMinInterval;
    /** Private engine (seeded from random_drop_seed) so the draw
     *  sequence is independent of the rest of the simulation */
    std::mt19937 randomDropRng;
    /** Scale drawn for the current epoch; 0 = drop-free (also the
     *  initial state: the first epoch never drops) */
    double currentDropScale = 0.0;
    /** Demand accesses observed since the last epoch draw */
    unsigned notifiesSinceDraw = 0;

    /**
     * One captured index line, latched raw at the fill. The index
     * line queue stages payloads exactly as GDP's does; conversion waits
     * for the drain (the lane array runs once per producer per event).
     */
    struct CapturedLine
    {
        /** Line VA of the index data: copied to the latch as the
         *  batch tag, and the debug-trace key */
        Addr lineVaddr = 0;
        /** Raw payload, sliced at the producer's EEW at conversion */
        std::vector<uint8_t> data;
        /** Conversion cursor: elements already through the lane
         *  array (a line wider than conversion_lanes resumes here).
         *  At most blkSize/EEW = 64 elements, so 8 bits. */
        uint8_t nextElem = 0;
        /** Elements the random tail-drop throttle truncated off the
         *  line's end (drawn once, at the line's first chunk); same
         *  8-bit element range as nextElem */
        uint8_t tailDropElems = 0;
        /** Strided (vlse) producers: bit k set = slot k (k*EEW bytes
         *  into the line) holds an index element; 0 = every slot
         *  (unit-stride line). Off-mask slots cost no lane. */
        uint64_t slotMask = 0;
    };

    /**
     * The lane array's output latch: one converted line's target
     * addresses, already line-deduplicated, draining into queue room
     * across events. One latch per producer slot — the array cannot
     * start the next line until it empties. The latch is live iff
     * its cursor is short of its count; there is no valid bit.
     */
    struct ConvertedLine
    {
        /** Line VA of the index data: the batch tag that
         *  drop_batch_confidence keys emittedFrom/abortedBatches on */
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
        /** Next target to emit (the emission cursor); bounded by the
         *  targets a single line can produce: up to 64 elements x
         *  consumers_per_producer ways (256 at e8 with 4 ways), so
         *  9 bits */
        uint16_t next = 0;
        /** This chunk finishes its source line: emitting it fully
         *  closes the line's dedup-window entries */
        bool lastChunk = false;

        bool pending() const { return next < targets.size(); }
        void clear() { targets.clear(); next = 0; }
    };

    /**
     * One consumer way of a producer: the dedup bookkeeping of one
     * linked gather. The raw index line queue is shared across ways
     * (it stages index data, which is base-agnostic); everything
     * downstream of the broadcast — the form, the seen-set, the
     * cross-line window — is per-way because the target spaces are
     * disjoint. The form itself lives in the IPT way of the same
     * index, where the lane array reads it; a way is live iff that
     * record holds a valid form (wayForm).
     */
    struct ConsumerGroup
    {
        /** The seen-set: folded index keys (preLaneKey), one per
         *  distinct target line, oldest first. Carries dedup across
         *  conversion_lanes chunks. Its scope is seen_set_entries:
         *  it clears at each captured line (0), or persists across
         *  lines as a FIFO CAM of that many keys (N > 0). */
        std::deque<Addr> lineSeen;
        /** Target lines EMITTED from the current source line; closes
         *  into one dedup-window entry with the line's last chunk
         *  (only maintained when dedup_buffer_size > 0) */
        std::vector<Addr> pendingEmitted;
        /** Emitted target lines of the last dedup_buffer_size
         *  processed index lines, oldest first (the cross-line CAM;
         *  empty when dedup_buffer_size = 0) */
        std::deque<std::vector<Addr>> dedupWindow;
    };

    /**
     * Stream Tracking Table (STT) entry: per-PRODUCER runtime state,
     * keyed by the producer load's PC. Holds the stream walk cursor
     * and high-water mark plus the consumer ways; captured lines
     * stage in its index line queue and convert at the drain, and the
     * IRT routes each captured line back to its producer by this
     * table's key. Every entry is a unit-stride vector producer, and
     * an entry exists only once its PC has produced a chunk event —
     * its presence is its validity.
     *
     * Invariant: the index line queue and the latch are non-empty
     * only while the producer is live (holds an IPT set). Captures
     * are admitted only for live producers, and the invalidate that
     * frees a producer's last way flushes both.
     */
    struct SttEntry
    {
        /** Demand cursor: start of the most recent chunk seen at this
         *  producer's PC. Chunk detection compares against it and a
         *  backward jump resets the walk. */
        Addr demandAddr = 0;
        /** Stream high-water mark (exclusive end of issued window) */
        Addr limitAddr = 0;
        /** Consumer ways, indexed by IPT way (sized once, to
         *  consumers_per_producer) */
        std::vector<ConsumerGroup> groups;
        /** Captured raw index lines awaiting conversion (shared:
         *  the broadcast source for every way's lane group) */
        std::deque<CapturedLine> indexLineQueue;
        /** The lane array's output latch */
        ConvertedLine latch;
        /** Strided (vlse) producer span model (2026-09-12). The ISA
         *  issues a vlse as one EEW-byte access per element, so a
         *  chunk is the span [chunkStart, chunkEnd) = vl elements at
         *  `stride` bytes; an access inside it is the same chunk.
         *  phase = chunkStart mod stride locates the element grid
         *  inside a line (slot masks derive from phase + stride);
         *  stride == 0 means unit-stride (all fields unused). */
        int64_t stride = 0;
        unsigned strideElemBytes = 0;
        unsigned stridePhase = 0;
        Addr chunkStart = 0;
        Addr chunkEnd = 0;
        /** Segment (vlseg) producer (2026-09-16): field count nf
         *  (0/1 = unit-stride), slice width, and the macro base the
         *  chain table captured at commit — the phase reference of
         *  the field slot masks. Memory-side a vlseg IS a unit-stride
         *  stream (contiguous requests over the interleaved region),
         *  so the walk/chunk model above is unchanged; only the
         *  captured line's slot mask differs. Refreshed from
         *  ProducerInfo at every access. */
        uint8_t nfields = 0;
        uint8_t segElemBytes = 0;
        bool segBaseValid = false;
        Addr segBase = 0;
    };
    std::unordered_map<Addr, SttEntry> streamTrackingTable;

    /** Index Routing Table entry: an index line registered for
     *  capture, with producerPC as the routing destination (a
     *  stream tracking table key) */
    struct IrtEntry
    {
        Addr linePaddr;
        Addr lineVaddr;
        Addr producerPC;
        bool secure;
        /** Slot mask for a strided producer's line (0 = all slots) */
        uint64_t slotMask = 0;
    };
    std::list<IrtEntry> indexRoutingTable;

    /**
     * Retired IRT record: an index line whose conversion has already
     * run, kept after the pending entry is retired so a later read hit
     * on the same line can tell "this producer already converted these
     * elements" from "a sibling producer did". Only the key is kept -
     * no payload, no extent - so the tail is a small CAM, not a copy of
     * the IRT. FIFO to retired_tail_entries; a re-retirement refreshes
     * an existing record's position rather than duplicating it.
     */
    struct RetiredEntry
    {
        Addr linePaddr;
        Addr producerPC;
        bool secure;
    };
    std::list<RetiredEntry> retiredRoutingTail;


    /**
     * drop_batch_confidence bookkeeping. A "batch" is the set of
     * targets converted from ONE captured index line, keyed by that
     * line's VA (the latch carries it). emittedFrom tags each queued target line with its
     * source batch; the getPacket() peek latches the tag of the
     * packet being handed to the cache, and the pfHitInCache()
     * override (called synchronously if the cache drops it as
     * resident) attributes the drop. All structures are small bounded
     * FIFOs - stale tags age out; an aged-out abort simply lets a
     * genuine revisit re-emit, which is correct.
     */
    /** target line VA -> source index line VA (one-shot: erased at
     *  issue), bounded by emittedFromFifo */
    std::unordered_map<Addr, Addr> emittedFrom;
    std::deque<Addr> emittedFromFifo;
    /** Per-batch resident-drop tally (recent batches only) */
    struct BatchDropRec { Addr source; unsigned drops; };
    std::list<BatchDropRec> batchDropRecs;
    /** Batches past the confidence threshold: their still-unemitted
     *  targets are thinned at the push site. acc is the Bresenham
     *  accumulator that spreads drop_batch_fraction evenly over the
     *  batch's remaining targets (flush + suppression share it). */
    struct AbortedBatch { Addr source; double acc; };
    std::list<AbortedBatch> abortedBatches;
    /** Source batch of the packet getPacket() just handed over
     *  (invalid when it was a stream candidate) */
    bool lastIssuedValid = false;
    Addr lastIssuedSource = 0;


    /**
     * Eviction-clocked dedup (evict_dedup_entries != 0). A global
     * LRU table of emitted indirect target lines whose entries live
     * until the CACHE says the line is gone, instead of falling off
     * a fixed-depth window: suppression lasts exactly as long as
     * re-emission would be wasted, for any reuse distance — the
     * long-range recurrence the per-way dedup window cannot see.
     *
     *  - insert at emission (a target actually pushed to the queue);
     *  - refresh (LRU) on a suppression hit and on an observed demand
     *    HIT to the line — recurring resident lines stay tracked;
     *  - clear on notifyEvict (the line left the cache: re-arm) and
     *    on an observed demand MISS to the line (proof it is not
     *    resident — heals entries whose eviction was never seen,
     *    e.g. a prefetch dropped at translation);
     *  - on overflow, replace the least-recently-touched entry.
     *
     * Every approximation degrades toward emitting (a dropped entry
     * costs one redundant emission, squashed as pfHitInCache); the
     * only fail-unsafe window is an eviction landing between
     * suppression and the demand re-touch, bounded by the miss-clear.
     *
     * Entries are keyed by target line VA (the space conversion and
     * the demand stream share); the eviction probe reports PAs, so
     * the getPacket() peek — the one point holding a target's VA and
     * translated PA together — binds the PA to the entry, and
     * notifyEvict clears through that side index.
     */
    /** Base's alias is private; re-declare it (as sms.hh does) so the
     *  notifyEvict override below can name its argument type */
    using EvictionInfo = CacheDataUpdateProbeArg;

    struct EvictDedupEntry
    {
        Addr lineVaddr;
        Addr linePaddr = 0;
        bool paValid = false;
    };
    /** MRU at the front; the back is the replacement victim */
    std::list<EvictDedupEntry> evictDedupLru;
    std::unordered_map<Addr,
        std::list<EvictDedupEntry>::iterator> evictDedupByVa;
    std::unordered_map<Addr,
        std::list<EvictDedupEntry>::iterator> evictDedupByPa;


    /** Tracked? Then refresh its LRU position and return true (the
     *  emission-side suppression check). */
    bool evictDedupSuppress(Addr line_va);
    /** Track an emitted target line (LRU replacement on overflow) */
    void evictDedupInsert(Addr line_va);
    /** Bind the issued packet's PA to its entry (getPacket peek) */
    void evictDedupBindPa(Addr line_va, Addr line_pa);
    /** Demand access to line_va: hit refreshes, miss clears */
    void evictDedupObserveAccess(Addr line_va, bool miss);
    /** Erase one entry (both side indexes) */
    void evictDedupErase(std::list<EvictDedupEntry>::iterator it);

    struct ViperFinalStats : public statistics::Group
    {
        ViperFinalStats(statistics::Group *parent);
        /** Chunk events at producer PCs */
        statistics::Scalar chunksObserved;
        /** Index-array stream candidates emitted */
        statistics::Scalar streamCandidates;
        /** Walk lines suppressed at the announced extent end
         *  (limit_gate) */
        statistics::Scalar streamLimitClamped;
        /** Accesses whose entire walk was suppressed because no live
         *  announcement covered them (limit_gate) */
        statistics::Scalar streamLimitNoAnnounce;
        /** Folded forms the discovery half pushed in (a way became
         *  live, or a live way was refreshed) */
        statistics::Scalar formsInstalled;
        /** Pushes dropped: no IPT set free for that producer (the
         *  producer_slots cap) */
        statistics::Scalar iptSetConflicts;
        /** Pushes dropped: the producer's ways are all taken */
        statistics::Scalar iptWaysExhausted;
        /** Consumer ways dropped by a per-gather invalidate pulse */
        statistics::Scalar formsInvalidated;
        /** Broadcast invalidates taken (the table was cleared, or a
         *  changed operand's head could not be resolved) */
        statistics::Scalar globalInvalidates;
        /** Lines registered for capture (a departing stream prefetch
         *  inside a live producer's window) */
        statistics::Scalar capturesRegistered;
        /** Captured index-line fills converted */
        statistics::Scalar fillsCaptured;
        /** Resident index lines latched off the read port on a demand
         *  read hit (capture_on_read_hit) */
        statistics::Scalar hitsCaptured;
        statistics::Scalar stridedChunks;
        statistics::Scalar stridedHitsCaptured;
        statistics::Scalar stridedUnsupported;
        statistics::Scalar stridedWalkGated;
        /** Segment (vlseg) producers: index lines captured with a
         *  field base mask */
        statistics::Scalar segmentLinesCaptured;
        /** ...of those, captured before the chain table had the
         *  macro base (phase assumed 0) */
        statistics::Scalar segmentPhaseUnknown;
        /** Accesses at segment producers refused (segment_producers
         *  = false, or nf/EEW outside the 64-slot line model) */
        statistics::Scalar segmentUnsupported;
        /** (way, slot) pairs a way skipped because the slot belongs
         *  to another field (no lane, no budget) */
        statistics::Scalar fieldSlotsSkipped;
        /** Unit-stride chunk events at a lower address than the
         *  cursor that reset the walk (window high-water mark to 0) */
        statistics::Scalar walkRestarts;
        /** Segment producers: backward chunk events inside the
         *  current macro's span (vl x nf x EEW bytes) treated as LSQ
         *  fragment reordering of the macro's nf memory micros, not
         *  a restart -- the walk and its high-water mark stand */
        statistics::Scalar segmentReorderTolerated;
        /** Read-hit captures of a line the retired tail says this same
         *  producer already converted (counted whether or not
         *  capture_read_hit_own_producer suppressed them) */
        statistics::Scalar recapturesDetected;
        /** ...of those, the ones actually skipped
         *  (capture_read_hit_own_producer = false) */
        statistics::Scalar recapturesSuppressed;
        /** Capture admissions skipped at a congested prefetch queue
         *  (capture_queue_gate) */
        statistics::Scalar capturesGated;
        /** Resident-drops attributed to a batch
         *  (drop_batch_confidence) */
        statistics::Scalar batchDropSamples;
        /** Batches aborted at the confidence threshold */
        statistics::Scalar batchesAborted;
        /** Targets removed from the queue or suppressed at emission
         *  because their batch aborted */
        statistics::Scalar targetsFlushedOnDrop;
        /** Fills dropped: index line queue full */
        statistics::Scalar bufferBusyDrops;
        /** Fills refused and latched targets forfeited because the
         *  producer, or the one way that made them, holds no form */
        statistics::Scalar noFormSkips;
        /** Elements pushed through a shift-and-add lane */
        statistics::Scalar elementsConverted;
        /** Conversion events cut short by the lane budget (the line
         *  resumed on a later event) */
        statistics::Scalar conversionWidthLimited;
        /** Epoch scale redraws taken by the random tail-drop
         *  throttle's clock (fixed or geometric) */
        statistics::Scalar epochRedraws;
        /** Captured lines the random tail-drop throttle truncated */
        statistics::Scalar linesRandomTruncated;
        /** Elements forfeited off truncated lines' tails (never
         *  reached a shift/add lane) */
        statistics::Scalar elementsRandomDropped;
        /** Elements whose slot was compacted out: every live way's
         *  seen-set hit before the lanes (pre_lane_dedup), so no
         *  lane fired and no conversion_lanes slot was spent. At one
         *  way this counts the same events as targetsDeduplicated;
         *  with several ways it isolates the all-ways-aligned
         *  duplicates (the compaction the any-miss slot rule banks
         *  on). */
        statistics::Scalar slotsCompacted;
        /** Indirect target candidates generated */
        statistics::Scalar targetsGenerated;
        /** Targets dropped by the canonical-address filter */
        statistics::Scalar targetsFiltered;
        /** Elements dropped as duplicate target lines by the per-way
         *  seen-set, in either placement (within one source line, or
         *  within the persistent CAM at seen_set_entries > 0) */
        statistics::Scalar targetsDeduplicated;
        /** Persistent seen-set keys replaced FIFO because the CAM was
         *  full (seen_set_entries > 0 only) */
        statistics::Scalar seenSetReplaced;
        /** Targets dropped: line emitted within the dedup window */
        statistics::Scalar targetsCrossDeduplicated;
        /** Targets suppressed: line tracked by the eviction-clocked
         *  dedup (emitted earlier, not seen leave the cache since) */
        statistics::Scalar targetsEvictDeduplicated;
        /** Tracked lines cleared by a cache eviction (re-armed) */
        statistics::Scalar evictDedupEvictCleared;
        /** Tracked lines cleared by an observed demand miss (the
         *  corrective re-arm: the line was provably not resident) */
        statistics::Scalar evictDedupMissCleared;
        /** Tracked lines replaced at capacity (LRU-by-access) */
        statistics::Scalar evictDedupReplaced;
        /** Targets dropped at a full queue (drop_on_full) instead of
         *  displacing queued entries */
        statistics::Scalar targetsDroppedFull;
        /** Emission events paused by a full queue (the stall firing) */
        statistics::Scalar emissionDeferred;
        /** Events where the array's one-line conversion slot ended
         *  with further lines still buffered */
        statistics::Scalar emissionLineLimited;
        /** Self-clocked drain firings that ran the emission engine */
        statistics::Scalar selfDrainTicks;
    } viperStats;

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

    /** The form's index extension (sign/zero, decoded extCode) alone */
    uint64_t extendRaw(const Form &f, uint64_t raw) const;

    /** Apply the slice's extension, then the shift-and-add lane */
    Addr applyForm(const Form &f, uint64_t raw) const;

    /** Seen-set key: same-target-line iff equal. Extend the raw
     *  index, shift (wire select), fold the base's sub-line offset
     *  (6-bit constant add), truncate to line granularity — no
     *  full-width base add, so it can run before lane allocation.
     *  line = key + (base >> 6) exactly, so the key names the line. */
    Addr preLaneKey(const Form &f, uint64_t raw) const;

    /** Scan one way's dedup window for a target line */
    bool inDedupWindow(const ConsumerGroup &cg, Addr line) const;

    /** Drop every way's per-line bookkeeping (seen-set and the
     *  pending window contribution): a source line was discarded. */
    void clearLineState(SttEntry &ps);
    /** Empty every way's seen-set outright, whatever its scope: the
     *  forms its keys were folded under are gone (producer flush) or
     *  the walk restarted (backward jump). */
    void clearSeenSets(SttEntry &ps);
    /** Empty one way's persistent seen-set (no-op at the per-line
     *  scope): a form push gave the way operands its keys were not
     *  folded under. */
    void clearWaySeenSet(Addr producer_pc, unsigned way);
    /** A source line finished: each way's emitted lines close into
     *  one dedup-window entry (empty ones too — the window ages by
     *  processed lines). No-op bookkeeping when the window is off. */
    void closeLineDedup(SttEntry &ps);
    /** Flush a producer's conversion pipeline: the index line queue,
     *  the latch, and every way's per-line bookkeeping. Run when the
     *  producer's last live way is invalidated. */
    void flushConversion(SttEntry &ps);

    /** The architectural stream: emit the lines of the window
     *  [addr + prefetch_distance*blkSize, +size) that lie above the
     *  high-water mark, then raise the mark. announced_limit != 0
     *  caps the walk at the announced extent end (limit_gate). */
    void streamAhead(SttEntry &ps, Addr addr, unsigned size,
                     Addr announced_limit,
                     std::vector<AddrPriority> &addresses);

    /** Strided producer walk: the window is the span
     *  prefetchDistance * (blkSize/EEW) elements ahead of the
     *  current chunk start and vl elements wide (the same BYTES of
     *  index data ahead as the unit-stride window); it emits the
     *  lines that span touches (every line for stride < blkSize,
     *  one per element otherwise) above the high-water mark. */
    void streamAheadStrided(SttEntry &ps,
                            std::vector<AddrPriority> &addresses);

    /** Slot mask of one line under a strided producer: bit k set for
     *  every k with k*width == phase_L + j*stride < blkSize, where
     *  phase_L = (chunkStart - line) mod stride is the offset of the
     *  line's first element. Elements are EEW-aligned (stride is a
     *  multiple of EEW, checked at admission). */
    uint64_t strideSlotMask(Addr line, unsigned width, int64_t stride,
                            Addr chunk_start) const;
    /** Field-0 base mask of one captured line of a segment (vlseg)
     *  producer: the element grid is base + j*(nfields*width), so
     *  in closed form a fixed pattern with one bit every nfields
     *  slots, shifted by the line's phase in slots (a constant per
     *  producer when the stride divides the line). */
    uint64_t segmentSlotMask(Addr line, unsigned width, unsigned nfields,
                             Addr base) const;
    /** The mask of field `field` from the field-0 base mask: the
     *  base mask rotated by `field` within the nfields-slot period
     *  (bit i set = slot i holds a field-`field` element). */
    static uint64_t fieldSlotMask(uint64_t base_mask, unsigned nfields,
                                  unsigned field);
    /** The slot mask a captured line of this producer carries:
     *  strided -> the vlse grid mask, segment -> the field-0 base
     *  mask, unit-stride -> 0 (every slot). */
    uint64_t captureSlotMask(SttEntry &ps, Addr line_va);

    /** Strided read-hit capture: the hitting access carries EEW
     *  bytes, not the line, so the resident line is read off the
     *  cache (accessor.readLine) and latched under the slot mask; the
     *  registration retires at once so the other vl-1 element hits on
     *  the line read as recaptures. */
    void captureStridedReadHit(SttEntry &ps, const PrefetchInfo &pfi,
                               Addr pc, Addr addr, bool is_secure,
                               const CacheAccessor &cache);


    /** Register one index line for capture at fill (bounded FIFO);
 */
    void registerCapture(Addr line_pa, Addr line_va, Addr producer_pc,
                         bool secure, uint64_t slot_mask = 0);

    /** Drop a registered line, so a capture taken elsewhere (the read
     *  port) is not latched a second time by a later fill */
    void dropRegistration(Addr line_pa, bool secure);

    /** Record that producer_pc's conversion of this line has run.
     *  Refreshes an existing record instead of duplicating it; FIFO to
     *  retired_tail_entries. No-op when the tail is disabled. */
    void retireRegistration(Addr line_pa, Addr producer_pc, bool secure);

    /** Does the retired tail say producer_pc already converted this
     *  line? */
    bool wasConvertedBy(Addr line_pa, Addr producer_pc,
                        bool secure) const;

    /** capture_queue_gate: is the prefetch queue backlog (queued
     *  targets + targets awaiting translation) at or above the
     *  admission threshold? Always false when the gate is disabled. */
    bool captureBacklogged() const;

    /** drop_batch_confidence: the cache just dropped the packet
     *  getPacket() handed it, because the line is already resident.
     *  Attribute the drop to its batch; at the threshold, flush the
     *  batch's queued targets and suppress its unemitted ones. */
    void pfHitInCache() override;

    /** capture_on_read_hit: latch the resident index lines this demand
     *  read hit on straight into the producer's index line queue, reading
     *  the payload off hitPkt (no IRT round trip — a hit holds the
     *  bytes, the VA and the PA at once). The caller has checked the
     *  producer is live. */
    void captureFromReadHit(SttEntry &ps, const PrefetchInfo &pfi,
                            Addr pc, Addr addr, unsigned size,
                            bool is_secure);



    /** Smallest announced extent end above line_va for this producer
     *  PC (0 = none / gate off; same aliasing rule as vhybrid.cc) */
    Addr announcedLimit(Addr pc, Addr line_va) const;

    /**
     * Convert the next up-to-conversion_lanes elements of the line
     * into the producer's output latch (0 = the whole line in one
     * event, every element through its own shift-and-add lane — the
     * classic parallel array; GDP walks the same payload
     * element-serially instead). Returns true when the line is
     * exhausted.
     */
    bool convertChunk(Addr pc, SttEntry &ps, CapturedLine &cap);

    /**
     * Release converted targets into free prefetch-queue slots, at
     * least drainFloor per event; called on every observed access,
     * which supplies the translation context.
     */
    void drainEmission(std::vector<AddrPriority> &addresses);

  public:
    ViperFinal(const ViperFinalPrefetcherParams &p);
    ~ViperFinal() = default;

    /** Latch the drain's translation context, then delegate; arms the
     *  self-clocked drain afterwards */
    void notify(const CacheAccessProbeArg &acc,
                const PrefetchInfo &pfi) override;

    void calculatePrefetch(const PrefetchInfo &pfi,
                           std::vector<AddrPriority> &addresses,
                           const CacheAccessor &cache) override;

    /** Observe issuing prefetch packets: register lines that fall in a
     *  live producer's stream window (range-based capture) */
    PacketPtr getPacket() override;

    void notifyFill(const CacheAccessProbeArg &acc) override;

    /** Eviction-clocked dedup: a line leaving the attached cache
     *  re-arms its tracked entry (the probe reports PAs; entries are
     *  matched through the PA side index bound at issue) */
    void notifyEvict(const EvictionInfo &info) override;

    void resetLearnedState() override;
};

} // namespace prefetch
} // namespace gem5

#endif //__MEM_CACHE_PREFETCH_VIPER_FINAL_HH__
