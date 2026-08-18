/**
 * RevelaStreamTable — the shared CPU-to-prefetcher channel of the
 * ReVeLA prefetcher (mem/cache/prefetch/revela.hh), following the
 * VectorChainTable sideband pattern (cpu/vector_chain_table.hh).
 *
 * ReVeLA ("Register Vector Length Agnostic prefetcher", Martinez
 * Palau et al., ICS'24) exploits the program semantics of Vector
 * Length Agnostic codes. A strip-mined loop passes the REMAINING
 * element count to vsetvl (the Requested Vector Length, RVL) and is
 * granted vl = min(RVL, VLMAX) (the Granted Vector Length, GVL). A
 * unit-stride access at access@ therefore announces, with certainty,
 * that the region [access@ + GVL, access@ + RVL) will be accessed by
 * the following loop iterations — a perfect prefetch candidate.
 *
 * Structures held here (paper Section 3.1):
 *
 *  - RVL shadow: the AVL of the last vset{i}vl{i} issued, snooped off
 *    the operand read the issue stage already performs (the paper
 *    propagates RVL/GVL with every memory request; one shadow
 *    register models that exactly for strip-mined loops, where every
 *    iteration re-executes vsetvl before its loads/stores).
 *  - STT (Stream Tracking Table, stt_entries=16): one entry per
 *    stream {current@, limit@, prefetched-up-to, valid, LRU}. Looked
 *    up by the stream LIMIT address (access@ + RVL), which is
 *    loop-invariant: every iteration's access@ + remaining bytes
 *    lands on the same end-of-array address. prefetched-up-to is
 *    kept as an address instead of the paper's prefetched_distance
 *    line count — the same information, but independent of the cache
 *    line size, which the CPU side does not know.
 *
 * Update rules (paper Section 3.2) run once per unit-stride vector
 * macro-op (micro-op 0) at LSQ translation finish:
 *  - forward hit (access@ >= current@): current@ <- access@ + GVL,
 *    LRU <- 0, all other entries age; current@ reaching limit@ is the
 *    stream's final access and invalidates the entry.
 *  - backward hit: ignored (out-of-order reordering of accesses in
 *    the same stream; updating would store outdated data).
 *  - miss: allocate over an invalid entry or the largest-LRU victim —
 *    unless the stream has no prefetchable data left (RVL <= GVL), in
 *    which case a dead entry would only depress the aggressivity.
 *  - a counter reaching lru_max invalidates its entry, cleaning up
 *    stale streams (e.g. last-two-accesses reordered at the end of a
 *    stream, which re-allocates an already-finished stream).
 *
 * All state clears at every stats reset (ROI boundaries), matching
 * the rest of this fork's prefetcher state.
 */

#ifndef __CPU_REVELA_TABLE_HH__
#define __CPU_REVELA_TABLE_HH__

#include <cstdint>
#include <vector>

#include "base/statistics.hh"
#include "base/types.hh"
#include "cpu/static_inst.hh"
#include "sim/sim_object.hh"

namespace gem5
{

struct RevelaStreamTableParams;

class RevelaStreamTable : public SimObject
{
  public:
    struct SttEntry
    {
        bool valid = false;
        /** First stream address not yet covered by a demand access:
         *  the last access@ + GVL bytes */
        Addr currentAddr = 0;
        /** End of the stream (access@ + RVL bytes): the loop-invariant
         *  lookup key */
        Addr limitAddr = 0;
        /** End of the already-prefetched region — [currentAddr,
         *  prefetchedUpTo) has been requested; the paper's
         *  prefetched_distance in address form */
        Addr prefetchedUpTo = 0;
        /** Aged by every other entry's update; lru_max = stale */
        unsigned lru = 0;
        /** PC of the last access that updated this entry (stamped at
         *  allocation and every forward hit). Consumers that need a
         *  producer/consumer identity for the stream (vhybrid.hh)
         *  resolve it through this PC on use — the STT itself stores
         *  no classification. */
        Addr lastPc = 0;
    };

    RevelaStreamTable(const RevelaStreamTableParams &p);

    /** Issue-stage snoop: a vset{i}vl{i} requested this AVL
     *  (in elements). */
    void notifyVsetvl(uint64_t avl_elems);

    /** vsetvl with rs1=x0, rd!=x0 requests VLMAX: the code reveals no
     *  stream end, so the shadow must not fabricate one. */
    void notifyVsetvlUnbounded();

    /**
     * LSQ hook, called at translation finish of every load/store
     * micro-op: the STT update of paper Section 3.2. Ignores
     * everything but micro-op 0 of unit-stride vector loads/stores.
     */
    void notifyVectorAccess(const StaticInst *si, Addr pc, Addr vaddr);

    /** The prefetcher's trigger logic walks (and advances) the
     *  entries directly. */
    std::vector<SttEntry> &entries() { return stt; }

    /** Valid-entry count, the aggressivity table's input. */
    unsigned numValid() const;

    /** Wipe STT and RVL shadow (ROI reset). */
    void resetState();

  private:
    const unsigned lruMax;

    /** RVL shadow (see file header) */
    bool avlKnown = false;
    uint64_t avlElems = 0;

    std::vector<SttEntry> stt;

    /** Age every valid entry but `except`; lru_max invalidates. */
    void ageOthers(int except);

    struct RevelaTableStats : public statistics::Group
    {
        RevelaTableStats(statistics::Group *parent);
        /** AVLs snooped at vset{i}vl{i} issue */
        statistics::Scalar vsetvlSnooped;
        /** vsetvl rs1=x0/rd!=x0 (VLMAX request, shadow invalidated) */
        statistics::Scalar vsetvlUnbounded;
        /** STT entries allocated (new streams) */
        statistics::Scalar streamsAllocated;
        /** Allocations skipped because RVL <= GVL (nothing left) */
        statistics::Scalar allocationsSkipped;
        /** Forward hits that advanced current@ */
        statistics::Scalar sttUpdates;
        /** Hits below current@ (reordered past-region accesses) */
        statistics::Scalar backwardAccesses;
        /** Streams invalidated by their final access */
        statistics::Scalar streamsCompleted;
        /** Entries invalidated by LRU saturation (stale streams) */
        statistics::Scalar staleInvalidations;
    } tableStats;
};

} // namespace gem5

#endif // __CPU_REVELA_TABLE_HH__
