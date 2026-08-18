/**
 * GDP — Gather Dataflow Prefetcher (cache side). Prefetches
 * A[f(B[i])] vector gathers with no training: the dataflow is
 * extracted architecturally. The CPU side (cpu/vector_chain_table.hh) records
 * producer heads, transform links and gather links (DCT) and forms
 * producer->gather links in one iteration (backward propagation at
 * the gather's dispatch). This prefetcher runs the runtime dataflow:
 *
 *  - every observed access at a producer PC advances that producer's
 *    architectural stream walk and prefetches the index array
 *    streaming_distance lines ahead (confidence-free: unit-stride
 *    identity comes from the opcode, so the walk is exact from the
 *    first access — no training, no constant-delta assumption);
 *  - if the producer is gather-linked, the chunk event adopts the
 *    producer's replay-pipeline configuration (transform ops in
 *    head-to-gather order + the dedicated base-adder operand). The
 *    table builds it on demand with a bounded consumer->head walk
 *    ("chain dispatch"); the adopted copy in the Stream Tracking
 *    Table (STT) entry is the only latched one — in hardware, the
 *    backprop walk distributes ops directly into these pipeline
 *    stage registers. Per-producer pipelines, capped by the
 *    pipelines param;
 *  - index-line data is captured by RANGE, not per-line tags: a
 *    prefetch about to issue (getPacket override peeks the deferred
 *    packet, whose PrefetchInfo holds the VA and whose pkt the
 *    translated PA — the issued request itself is PA-only) or a demand
 *    miss whose line falls inside a configured producer's stream
 *    window registers that line's PA in the Index Routing Table
 *    (IRT); notifyFill matches the PA against it and routes the
 *    payload into that producer's slice buffer. Because the stream runs D lines
 *    ahead, the capture inherits the stream's lookahead — the
 *    indirect targets are generated D chunks before their demand
 *    (timeliness needs D*cadence >= 2*memory latency: index line AND
 *    target line each round-trip);
 *  - replay (STALL-ON-FULL, demand-clocked) slices
 *    the captured payload at the producer's EEW, streams each element
 *    (widened per-element to 64 bits — the architectural 2xVLEN
 *    intermediate never materializes) through the latched transform
 *    ops, adds the base, and emits one prefetch per distinct target
 *    line — into free prefetch-queue slots, with a small per-event
 *    floor (drainFloor) of bounded displacement when the queue is
 *    full, modeling hardware's continuous queue drain. The captured
 *    payload waits IN the slice buffer (denser than expanded
 *    targets) and the drain resumes at the next observed access
 *    (which also supplies translation context). The hardware ITB is
 *    thereby a conversion-stage output register per pipeline, not a
 *    buffer;
 *  - replay WIDTH is pipelines_per_gather: that many copies of the
 *    transform chain sit behind one gather, all fed from the same
 *    slice-buffer entry, so N consecutive indices convert together
 *    rather than one per drain event. This is the only throughput
 *    term in the replay model — every other bound here is capacity
 *    (queue slots, buffer entries). Width is PER GATHER, since the
 *    pipelines are private to it: one producer exhausting its
 *    pipelines does not stall another's, unlike the shared queue
 *    budget, which stops the whole drain. An element that is filtered
 *    or line-deduplicated still occupied a pipeline; only emitted
 *    targets consume queue slots. 0 = unbounded, the legacy model in
 *    which replay width never binds. Setting it to blkSize/EEW is the
 *    whole-line-per-event limit — the point at which GDP's serial
 *    chain matches VTyche's collapsed one-shot conversion
 *    (mem/cache/prefetch/vector_tyche.hh), making the two directly
 *    comparable on conversion throughput as well as on algebra.
 *
 * Load shedding under sustained overload happens at fill admission,
 * chunk-granular: each replay pipeline has ONE slice buffer of
 * slice_buffer_entries line entries, and a fill arriving with the
 * buffer full is dropped (bufferBusyDrops) — all-or-nothing per index
 * line, matching vector-gather completion semantics (one uncovered
 * line stalls the whole gather anyway). A STALENESS ABORT flushes
 * entries whose index line the producer's walk cursor has already
 * passed
 * (stalenessAborts) so stalls never rot into late prefetches. This
 * back-pressure discipline mirrors Ara's own addrgen, which stalls on
 * a full address queue rather than dropping.
 *
 * No confidence and no kill switch: transforms are replayed exactly,
 * so systematic mispredictions cannot arise from the A[f(B[i])] shape
 * itself; wrong-path pollution self-heals because every gather
 * dispatch re-walks its link.
 */

#ifndef __MEM_CACHE_PREFETCH_GDP_HH__
#define __MEM_CACHE_PREFETCH_GDP_HH__

#include <deque>
#include <list>
#include <unordered_map>
#include <vector>

#include "base/statistics.hh"
#include "cpu/vector_chain_table.hh"
#include "mem/cache/prefetch/queued.hh"

namespace gem5
{

struct GDPPrefetcherParams;

namespace prefetch
{

class GDP : public Queued
{
    /** The CPU-side chain/link table (shared SimObject) */
    VectorChainTable *const tbl;
    /** Stream lookahead in lines (sets indirect lookahead too) */
    const int streamingDistance;
    /** Ablation: stream the index arrays but never capture/replay */
    const bool streamOnly;
    /** Line entries in each pipeline's slice buffer (capture
     *  concurrency: how many captured index lines can wait to drain) */
    const unsigned sliceBufferEntries;
    /** Concurrently configured producer pipelines */
    const unsigned pipelines;
    /** Replay pipelines behind ONE gather: elements of the head
     *  slice-buffer entry advanced per drain event, one per pipeline.
     *  0 = unbounded (the legacy model, replay width never binds). */
    const unsigned pipelinesPerGather;
    /** Index Routing Table capacity (registered awaiting-fill lines) */
    const unsigned irtEntries;

    /** One slice-buffer entry: a captured index-line payload, drained
     *  element-serially into free queue slots */
    struct CapturedLine
    {
        std::vector<uint8_t> data;
        /** Line VA, for the staleness abort vs the walk cursor */
        Addr lineVaddr = 0;
        /** Next element to replay (the drain cursor) */
        unsigned nextElem = 0;
        /** Target lines already emitted from this payload (dedup) */
        std::vector<Addr> emitted;
    };

    /** Per-producer runtime state (stream walk + latched pipeline) */
    struct SttEntry
    {
        Addr lastAddr = 0;
        bool valid = false;
        /** Stream high-water mark (exclusive end of issued window) */
        Addr limitAddr = 0;
        /** Latched replay configuration ("chain dispatch") */
        VectorChainTable::ChainSnapshot config;
        bool configured = false;
        /** Slice width latched with the config */
        unsigned elemBytes = 0;
        /** The pipeline's slice buffer: captured payloads awaiting
         *  drain, at most sliceBufferEntries of them */
        std::deque<CapturedLine> sliceBuffer;
    };
    std::unordered_map<Addr, SttEntry> streamTrackingTable;
    /** Producers currently holding a pipeline (vs pipelines cap) */
    unsigned configuredCount = 0;

    /** Index Routing Table entry: an index line registered for
     *  capture, with producerPC as the routing destination — a fill
     *  matching linePaddr is steered into that producer's slice
     *  buffer. lineVaddr rides along to the buffer, where the
     *  staleness abort compares it to the walk cursor. */
    struct IrtEntry
    {
        Addr linePaddr;
        Addr lineVaddr;
        Addr producerPC;
        bool secure;
    };
    std::list<IrtEntry> indexRoutingTable;

    struct GDPStats : public statistics::Group
    {
        GDPStats(statistics::Group *parent);
        /** Chunk events at producer PCs */
        statistics::Scalar chunksObserved;
        /** Index-array stream candidates emitted */
        statistics::Scalar streamCandidates;
        /** Pipeline configurations adopted at trigger */
        statistics::Scalar chainDispatches;
        /** Linked triggers whose chain was not yet snoopable */
        statistics::Scalar chainNotReady;
        /** Linked producers denied a pipeline (pipelines cap) */
        statistics::Scalar pipelinesSaturated;
        /** Lines registered for capture (prefetch-window path) */
        statistics::Scalar capturesRegistered;
        /** Lines registered for capture (demand-miss path) */
        statistics::Scalar capturesRegisteredMiss;
        /** Captured index-line fills replayed */
        statistics::Scalar fillsCaptured;
        /** Fills dropped: all slice buffers draining */
        statistics::Scalar bufferBusyDrops;
        /** Fills dropped: configuration stale (table cleared) */
        statistics::Scalar staleConfigs;
        /** Elements replayed through the pipeline */
        statistics::Scalar elementsReplayed;
        /** Indirect target candidates generated */
        statistics::Scalar targetsGenerated;
        /** Targets dropped by the canonical-address filter */
        statistics::Scalar targetsFiltered;
        /** Slice buffers flushed: walk cursor passed their line */
        statistics::Scalar stalenessAborts;
        /** Drain events paused by a full queue (the stall firing) */
        statistics::Scalar replayDeferred;
        /** Drain events where a gather's replay pipelines ran out with
         *  elements still buffered (width, not queue room, bound) */
        statistics::Scalar replayWidthLimited;
    } gdpStats;

    /** The architectural stream: emit lines in (highWater,
     *  addr + size*(D+1)], raise the mark. Never queue-gated —
     *  stream candidates are regenerable and feed every capture. */
    void streamAhead(SttEntry &ps, Addr addr, unsigned size,
                     std::vector<AddrPriority> &addresses);

    /** Register one index line for capture at fill (bounded FIFO) */
    void registerCapture(Addr line_pa, Addr line_va, Addr producer_pc,
                         bool secure, bool from_miss);

    /** Minimum drain emissions per access event when the queue is
     *  full (bounded displacement): models the continuous queue
     *  drain of real hardware, which gem5's pull-only queue lacks */
    static constexpr int drainFloor = 8;

    /** Drain captured payloads through the pipelines into free queue
     *  slots, at least drainFloor per event (the stall-on-full
     *  replay); called on every observed access, which supplies the
     *  translation context */
    void drainReplay(std::vector<AddrPriority> &addresses);

    /** Apply the latched transform ops + base adder to one element */
    Addr applyChain(const VectorChainTable::ChainSnapshot &cfg,
                    uint64_t value) const;

  public:
    GDP(const GDPPrefetcherParams &p);
    ~GDP() = default;

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

#endif //__MEM_CACHE_PREFETCH_GDP_HH__
