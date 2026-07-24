/**
 * Tyche — dependency-chain indirect prefetcher (cache side), a
 * gem5/RISC-V port of the Tyche artifact (~/Tyche-Artifact,
 * ChampSim/LoongArch). The CPU side (cpu/tyche_table.hh) builds
 * instruction chains rooted at IP-stride loads; this prefetcher runs
 * the runtime walk the artifact keeps in its AGQ + prefetcher_operate:
 *
 *  - every observed scalar access trains the shared IPT
 *    (TycheChainTable::observeAccess). When the artifact's conf_trigger
 *    fires, the head load is stride-prefetched stride_distance
 *    iterations ahead (tyche.cc:266-273).
 *  - if the head roots a replayable chain (chainTrigger), the stride
 *    target starts a *walk*: the predicted element address is recorded
 *    and the walk waits for the prefetched line to fill.
 *  - walk prefetches are emitted as virtual addresses and translated by
 *    the queued-prefetcher machinery; the getPacket() override peeks
 *    each deferred packet as it issues (its PrefetchInfo holds the VA,
 *    its pkt the translated PA — the issued request itself is PA-only)
 *    and learns the line's PA, which is what notifyFill() sees.
 *  - on the fill, the loaded value is read straight from the fill
 *    payload (replacing the artifact's shadow-memory read in
 *    idm_load_return), the chain's ALU links replay on it
 *    (TycheChainTable::executeAlu), and each dependent load link yields
 *    the next prefetch address = link constant + propagated value
 *    (tyche.cc:99). Loads with further replayable successors register a
 *    new walk step (multi-hop); fill-computed targets are buffered and
 *    flushed by the next observed access, which supplies the
 *    translation context fills lack (same pattern as GDP).
 *
 * Deviations from the artifact (documented in DOCUMENTATION.MD): the
 * software ALU executes synchronously at fill time instead of 2-3
 * cycles per op through a cycle_operate loop, and a walk whose
 * intermediate prefetch never issues or fills (line already cached,
 * queue squash) dies silently — the artifact leaks the AGQ entry until
 * a wholesale clear, with the same net effect. Walk state referring to
 * chain links from before a DCT clear is discarded via the table's
 * generation counter.
 */

#ifndef __MEM_CACHE_PREFETCH_TYCHE_HH__
#define __MEM_CACHE_PREFETCH_TYCHE_HH__

#include <list>
#include <vector>

#include "base/statistics.hh"
#include "cpu/tyche_table.hh"
#include "mem/cache/prefetch/queued.hh"

namespace gem5
{

struct TychePrefetcherParams;

namespace prefetch
{

class Tyche : public Queued
{
    /** The CPU-side chain table (shared SimObject) */
    TycheChainTable *const tct;
    /** Head-load stride lookahead in iterations (artifact
     *  L1_STRIDE_DISTANCE) */
    const unsigned strideDistance;
    /** Disable the chain walk, keeping only the IP-stride prefetches
     *  (artifact only_stride ablation) */
    const bool strideOnly;
    /** In-flight walk-step capacity (artifact AGQ_SIZE) */
    const unsigned walkEntries;
    /** Chain successors woken per returned value (artifact
     *  ISQ_WRITE_PORT) */
    const unsigned successorsPerWakeup;
    /** Total ALU replays allowed per captured fill (cycle guard) */
    const unsigned maxChainHops;
    /** Buffered fill-computed target capacity */
    const unsigned pendingTargetEntries;

    /** One in-flight walk step: a chain load whose fill we await */
    struct WalkEntry
    {
        /** Predicted element address (virtual, element-precise) */
        Addr targetVa;
        /** Its cache line (virtual), matched at getPacket() */
        Addr lineVa;
        /** Its cache line (physical), matched at notifyFill();
         *  0 until the prefetch packet has been observed issuing */
        Addr linePa = 0;
        /** DCT link of the load this step replays */
        int dctPtr;
        /** Chain-table generation this step belongs to */
        uint64_t gen;
        bool secure;
        bool awaitingFill = false;
    };
    std::list<WalkEntry> walks;

    /** Fill-computed target VAs awaiting the next access's context */
    std::vector<Addr> pendingTargets;

    struct TychePfStats : public statistics::Group
    {
        TychePfStats(statistics::Group *parent);
        /** IP-stride prefetch candidates emitted */
        statistics::Scalar strideCandidates;
        /** Walks started at a chain head */
        statistics::Scalar walksStarted;
        /** Oldest walk step evicted: walk list full (AGQ_FULL) */
        statistics::Scalar walksDroppedFull;
        /** Walk steps whose prefetch was observed issuing (VA->PA) */
        statistics::Scalar walksIssued;
        /** Walk-step fills captured and replayed */
        statistics::Scalar fillsCaptured;
        /** ALU link replays executed */
        statistics::Scalar aluReplays;
        /** Chain prefetch candidates generated (dependent loads) */
        statistics::Scalar chainCandidates;
        /** Chain targets dropped by the canonical-address filter
         *  (artifact AGQ_BEYOND) */
        statistics::Scalar chainTargetsFiltered;
        /** Fill-computed targets flushed into the candidate list */
        statistics::Scalar pendingFlushed;
        /** Walk steps discarded: chain table cleared under them */
        statistics::Scalar walksStale;
    } tycheStats;

    /** Record a walk step awaiting issue+fill of its line (bounded) */
    void registerWalk(Addr target_va, int dct_ptr, bool secure);

    /**
     * Replay the chain below one load link on its loaded value:
     * ALU links execute; dependent load links emit prefetch targets
     * (buffered) and, if they have replayable successors themselves,
     * register the next walk step.
     */
    void chase(int dct_ptr, uint64_t value, bool secure);

  public:
    Tyche(const TychePrefetcherParams &p);
    ~Tyche() = default;

    void calculatePrefetch(const PrefetchInfo &pfi,
                           std::vector<AddrPriority> &addresses,
                           const CacheAccessor &cache) override;

    /** Observe issuing prefetch packets to learn walk lines' PAs */
    PacketPtr getPacket() override;

    void notifyFill(const CacheAccessProbeArg &acc) override;

    void resetLearnedState() override;
};

} // namespace prefetch
} // namespace gem5

#endif //__MEM_CACHE_PREFETCH_TYCHE_HH__
