/**
 * ReVeLA — the Register Vector Length Agnostic prefetcher (Martinez
 * Palau et al., "Exploiting Vector Code Semantics for Efficient Data
 * Cache Prefetching", ICS'24), the cache-side half of the design.
 *
 * The CPU-side half is the RevelaStreamTable (cpu/revela_table.hh):
 * the Stream Tracking Table (STT), fed by the issue stage (vsetvl AVL
 * snoop) and the LSQ (unit-stride vector access reports). Each valid
 * STT entry names a stream the program has ANNOUNCED it will access —
 * [current@, limit@) — because the vsetvl AVL (Requested Vector
 * Length) exceeded the granted vl. This side turns those entries into
 * prefetch requests.
 *
 * Trigger logic (paper Section 3.3): unlike every other prefetcher in
 * this tree, ReVeLA is NOT driven by cache events. A self-clocked
 * drain event (every cycle, the paper's fixed cadence — the same
 * machinery as vector_tyche2.hh's drain, but not parameterized: any
 * other period would be a different design) evaluates the trigger
 * whenever the prefetch queue has room:
 *
 *  - the Aggressivity Table caps the prefetch distance based on how
 *    many streams are live, splitting the in-flight budget:
 *    <2 valid entries -> max_prefetch_distance lines, <4 -> /2,
 *    <8 -> /4, else /8 (the paper's 64/32/16/8 with the default 64).
 *  - among eligible entries (valid, not past limit@, not above the
 *    aggressivity cap), only those at the MINIMUM prefetched distance
 *    emit — round-robin fairness in space: streams advance together,
 *    degree lines each per firing.
 *  - emitted lines land in the standard Queued prefetch queue
 *    (queue_size=16 to match the paper), and the cache pulls from it
 *    only when it has MSHR headroom (BaseCache::getNextQueueEntry
 *    checks mshrQueue.canPrefetch(); the paper's ">= 8 free MSHRs"
 *    rule maps onto the cache's demand_mshr_reserve parameter).
 *
 * Addresses are VIRTUAL (paper Section 3.5): streams are
 * VA-contiguous but not PA-contiguous across page frames, so the STT
 * tracks VAs and every prefetch translates through the registered MMU
 * (use_virtual_addresses; page-crossing requests need the MMU or they
 * are dropped). Like the demand path, this can prefetch TLB entries.
 *
 * Like the paper's design, ReVeLA is meant to COMPLEMENT a coverage
 * prefetcher, not replace one: it only ever requests lines the
 * program has already promised to touch, so its accuracy is ~100% by
 * construction while its coverage is limited to announced streams.
 */

#ifndef __MEM_CACHE_PREFETCH_REVELA_HH__
#define __MEM_CACHE_PREFETCH_REVELA_HH__

#include <memory>
#include <vector>

#include "cpu/revela_table.hh"
#include "mem/cache/prefetch/queued.hh"

namespace gem5
{

struct RevelaPrefetcherParams;

namespace prefetch
{

class ReVeLA : public Queued
{
    /** CPU-side STT (shared with BaseCPU.revela_table) */
    RevelaStreamTable *const tbl;

    /** Aggressivity Table ceiling: max prefetched lines ahead of
     *  current@ when fewer than 2 streams are live; /2, /4, /8 as the
     *  valid-entry count crosses 2, 4, 8 (paper Figure 5) */
    const unsigned maxPrefetchDistance;

    /** Lines emitted per min-distance stream per trigger evaluation
     *  (the paper emits 1 per cycle per stream) */
    const unsigned degree;

    /** Emission floor in lines ahead of current@: at stream entry and
     *  after every demand catch-up the frontier jumps to current@ +
     *  initialDistance lines instead of ramping from current@ — the
     *  distance-zero region is doomed-late (demand reaches it well
     *  inside one memory round trip). Skipped lines are never
     *  prefetched; demand pays their full miss. VTyche's
     *  streaming_distance analogue; 0 = paper behavior. */
    const unsigned initialDistance;

    /**
     * Latched translation context for the self-clocked drain, exactly
     * as vector_tyche2.hh does it: insert() needs a request with a VA
     * and a ContextID plus the cache accessor, all captured from the
     * last observed demand access.
     */
    RequestPtr drainCtxReq;
    std::unique_ptr<PrefetchInfo> drainCtxPfi;
    const CacheAccessor *drainCtxCache = nullptr;

    EventFunctionWrapper drainEvent;

    /** Current aggressivity cap (lines) for the live stream count */
    unsigned aggressivityLimit() const;

    /** Lines already prefetched ahead of current@ (the paper's
     *  prefetched_distance) */
    unsigned distanceLines(const RevelaStreamTable::SttEntry &e) const;

    /** Jump a stream's frontier to the initial-distance floor
     *  (no-op at initial_distance=0) */
    void applyInitialDistance(RevelaStreamTable::SttEntry &e);

    /** One trigger evaluation (paper Figure 7): pick the min-distance
     *  eligible streams and emit up to degree lines each, bounded by
     *  free queue room. Advances the STT's prefetched-up-to marks. */
    void emitRound(std::vector<AddrPriority> &addresses);

    /** True while some stream is eligible to emit */
    bool workPending() const;

    void scheduleDrain();
    void drainTick();

    struct RevelaStats : public statistics::Group
    {
        RevelaStats(statistics::Group *parent);
        /** Self-clocked drain firings that evaluated the trigger */
        statistics::Scalar selfDrainTicks;
        /** Prefetch lines emitted into the queue */
        statistics::Scalar linesEmitted;
        /** Trigger evaluations skipped: no free queue room */
        statistics::Scalar drainNoRoom;

        statistics::Scalar linesSkippedFloor;
        /** Stream-emissions skipped by the aggressivity cap */
        statistics::Scalar aggressivityThrottled;
    } revelaStats;

  public:
    ReVeLA(const RevelaPrefetcherParams &p);
    ~ReVeLA() = default;

    void calculatePrefetch(const PrefetchInfo &pfi,
                           std::vector<AddrPriority> &addresses,
                           const CacheAccessor &cache) override;

    void notify(const CacheAccessProbeArg &acc,
                const PrefetchInfo &pfi) override;

    PacketPtr getPacket() override;
};

} // namespace prefetch
} // namespace gem5

#endif // __MEM_CACHE_PREFETCH_REVELA_HH__
