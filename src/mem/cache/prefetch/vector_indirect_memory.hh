/**
 * Implementation of the Vector Indirect Memory Prefetcher (VIMP)
 *
 * A generalization of the Indirect Memory Prefetcher (IMP) to vectorized
 * indirect access patterns, e.g. RISC-V RVV indexed loads (vluxei*).
 *
 * Reference for the scalar design this extends:
 * IMP: Indirect memory prefetcher.
 * Yu, X., Hughes, C. J., Satish, N., & Devadas, S. (2015, December).
 * In Proceedings of the 48th International Symposium on Microarchitecture
 * (pp. 178-190). ACM.
 *
 * Scalar code walks an index array B one element at a time (B[0], B[1],
 * B[2], ...), which is what IMP's stream detector and its one-index-per-
 * access pattern detector assume; IMP ignores accesses wider than 8 bytes
 * entirely. Vectorized code instead walks B one vector register at a time:
 * each loop iteration issues a single unit-stride load covering VLEN/EEW
 * elements. This prefetcher redefines the training unit from "element" to
 * "chunk" (one such wide access):
 *
 *  - Stream detection: a PC-indexed table tracks accesses where each new
 *    address equals the previous address plus the previous request size,
 *    i.e. back-to-back contiguous chunks. The chunk size is taken from the
 *    observed request, so any VLEN/LMUL/EEW works without configuration.
 *  - Index extraction: the payload of a chunk access is sliced into
 *    index_size-byte elements, yielding many index values per event.
 *  - Pattern detection: two misses of the same gather differ by exactly
 *    (idx_a << shift) - (idx_b << shift) for two distinct indices of the
 *    chunk that produced them. Each detector entry remembers the leading
 *    indices of the last ipd_chunk_history chunks plus a few recent miss
 *    addresses and searches miss-pair deltas for such an index pair,
 *    which also pins down baseAddr. Unlike IMP's idx1/idx2 phases —
 *    which assume misses arrive right after their index read — this is
 *    insensitive to the out-of-order core running the (always-hitting)
 *    index loads many iterations ahead of the MSHR-throttled gather
 *    misses.
 *  - Prefetch generation: for every index in the current chunk, prefetch
 *    baseAddr + (index << shift), deduplicated at cache-line granularity;
 *    the index array itself is stream-prefetched streaming_distance chunks
 *    ahead.
 *  - Lookahead (indirect_delta > 0): instead of the current chunk's own
 *    targets, issue targets for chunks up to indirect_delta ahead. Future
 *    index values only exist in the lines this prefetcher itself streams
 *    in, so they are captured when those lines *fill* the cache (the
 *    expected fill addresses are registered while stream-prefetching),
 *    buffered per stream, and released once the demand pointer is within
 *    indirect_delta chunks — the release goes through the normal
 *    calculatePrefetch path so page-crossing targets keep their
 *    translation context. Effective lookahead is therefore bounded by
 *    how far ahead the index array is streamed (streaming_distance).
 *
 * With index_size equal to the access size (scalar loads), chunks
 * degenerate to one index each. Stream detection and streaming still
 * work (stride == size), but pattern detection does not: the pair
 * invariant needs two distinct indices within one chunk. Use the stock
 * IMP as the scalar baseline.
 */

#ifndef __MEM_CACHE_PREFETCH_VECTOR_INDIRECT_MEMORY_HH__
#define __MEM_CACHE_PREFETCH_VECTOR_INDIRECT_MEMORY_HH__

#include <deque>
#include <list>
#include <vector>

#include "base/cache/associative_cache.hh"
#include "base/sat_counter.hh"
#include "base/statistics.hh"
#include "mem/cache/prefetch/queued.hh"
#include "mem/cache/tags/tagged_entry.hh"

namespace gem5
{

struct VectorIndirectMemoryPrefetcherParams;

namespace prefetch
{

class VectorIndirectMemory : public Queued
{
    /** Shift values considered when matching (baseAddr, shift) patterns */
    const std::vector<int> shiftValues;
    /** Confidence counter threshold to start indirect prefetching */
    const unsigned int prefetchThreshold;
    /** streamCounter value to trigger the chunk-stream prefetcher */
    const int streamCounterThreshold;
    /** Number of chunks prefetched ahead by the chunk-stream prefetcher */
    const int streamingDistance;
    /** Emit each stream line once per walk instead of re-emitting the
     *  whole window on every access (see streamIssuedUpTo) */
    const bool streamDedup;
    /** Size in bytes of one index element inside a chunk payload */
    const unsigned int indexSize;
    /** Whether index elements are sign-extended */
    const bool indexSigned;
    /** Maximum number of index elements sliced out of one chunk */
    const unsigned int maxIndicesPerChunk;
    /** Number of leading chunk indices used for IPD correlation */
    const unsigned int ipdIndicesPerChunk;
    /** Number of recent tracked misses an IPD entry keeps for pairing */
    const unsigned int addrArrayLen;
    /** Maximum indirect prefetches generated per chunk event */
    const unsigned int maxIndirectTargets;
    /** Correlate IPD candidates on hits too, not only on misses */
    const bool ipdTrainOnHits;
    /**
     * Number of recent chunks whose leading indices an IPD entry keeps
     * for miss correlation. Index-chunk reads run far ahead of the
     * gathers' element misses on an out-of-order core (the index loads
     * hit while the misses are MSHR-throttled), so a miss must be
     * matchable against chunks read many iterations earlier.
     */
    const unsigned int ipdChunkHistory;
    /**
     * Number of recent chunks whose indices an *enabled* entry keeps
     * for confidence matching. The gather accesses that confirm a
     * prediction lag the index-chunk reads by the same skew that
     * motivates ipdChunkHistory; matching them against only the latest
     * chunk's indices (disjoint from the lagging chunk's targets) would
     * starve the confidence counter and keep indirect prefetching
     * permanently disabled.
     */
    const unsigned int confidenceChunkHistory;
    /**
     * Consecutive chunks an enabled entry may spend at zero confidence
     * before it is demoted and allowed to retrain (0 disables demotion).
     */
    const unsigned int demotionChunks;
    /**
     * Lookahead distance in chunks for indirect targets. 0 issues the
     * current chunk's targets from its own payload; N > 0 issues targets
     * for chunks up to N ahead from fill-captured index lines.
     */
    const unsigned int indirectDelta;
    /** Capacity of the expected-fill table (index lines awaiting data) */
    const unsigned int pendingFillEntries;
    /** Captured index-line sets buffered per prefetch table entry */
    const unsigned int pendingIndexSets;

    /** Index values captured from one filled line of the index array */
    struct PendingIndexSet
    {
        /** Virtual address of the index-array line the values came from */
        Addr lineVaddr;
        /** The index values */
        std::vector<int64_t> indices;
    };

    /** Prefetch Table Entry */
    struct PrefetchTableEntry : public TaggedEntry
    {
        /* Chunk-stream table fields */

        /** Address of the last observed chunk */
        Addr address;
        /** Size in bytes of the last observed chunk */
        unsigned int lastSize;
        /** Whether this address is in the secure region */
        bool secure;
        /** Confidence counter of the chunk stream */
        unsigned int streamCounter;
        /**
         * First line address of the current walk NOT yet emitted as a
         * stream-prefetch candidate. The streaming window advances one
         * chunk per access but spans streamingDistance chunks, so
         * consecutive accesses re-visit almost every line; only lines
         * at or above this mark are emitted (when streamDedup is set).
         * Reset on discontinuity so a restarted walk re-prefetches.
         */
        Addr streamIssuedUpTo;

        /* Indirect table fields */

        /** Enable bit of the indirect fields */
        bool enabled;
        /**
         * Index values of the last confidenceChunkHistory chunks,
         * oldest first. Confidence matching searches all of them
         * because the confirming gather accesses arrive several
         * chunks behind the index reads.
         */
        std::deque<std::vector<int64_t>> recentIndices;
        /** BaseAddr detected */
        Addr baseAddr;
        /** Shift detected */
        int shift;
        /** Confidence counter of the indirect fields */
        SatCounter8 indirectCounter;
        /**
         * Set when at least one access matched a predicted target for
         * any index in recentIndices. Checked (and cleared) when the
         * next chunk arrives; if no match happened, the confidence
         * counter is decremented.
         */
        bool increasedIndirectCounter;
        /**
         * Consecutive chunks spent at zero confidence while enabled;
         * demotes the entry (allows retraining) at demotionChunks.
         */
        unsigned int noMatchChunks;
        /**
         * Index-line sets captured from fills of this stream's future
         * chunks, waiting for the demand pointer to come within
         * indirectDelta chunks (only used when indirectDelta > 0).
         */
        std::deque<PendingIndexSet> pendingChunks;

        PrefetchTableEntry(unsigned indirect_counter_bits, TagExtractor ext)
            : TaggedEntry(), address(0), lastSize(0), secure(false),
              streamCounter(0), streamIssuedUpTo(0), enabled(false),
              recentIndices(), baseAddr(0), shift(0),
              indirectCounter(indirect_counter_bits),
              increasedIndirectCounter(false), noMatchChunks(0),
              pendingChunks()
        {
            registerTagExtractor(ext);
        }

        void
        invalidate() override
        {
            TaggedEntry::invalidate();
            address = 0;
            lastSize = 0;
            secure = false;
            streamCounter = 0;
            streamIssuedUpTo = 0;
            enabled = false;
            recentIndices.clear();
            baseAddr = 0;
            shift = 0;
            indirectCounter.reset();
            increasedIndirectCounter = false;
            noMatchChunks = 0;
            pendingChunks.clear();
        }
    };
    /** Prefetch table */
    AssociativeCache<PrefetchTableEntry> prefetchTable;

    /**
     * Indirect Pattern Detector entry.
     *
     * The detector is not phase-clocked by index-chunk reads (IMP's
     * idx1/idx2 scheme): on an out-of-order core the (always-hitting)
     * index loads run many iterations ahead of the gathers' element
     * misses, so "the misses following a chunk read" belong to a much
     * older chunk and phase-aligned correlation never matches. Instead,
     * each entry keeps the leading indices of the last ipdChunkHistory
     * chunks and the last few tracked miss addresses, and exploits an
     * alignment-free invariant: two misses of the same gather differ by
     * exactly (idx_a << shift) - (idx_b << shift) for two distinct
     * indices of the one chunk that produced them.
     */
    struct IndirectPatternDetectorEntry : public TaggedEntry
    {
        /** Leading indices of recent chunks, oldest first */
        std::deque<std::vector<int64_t>> history;
        /** Recently tracked miss addresses, oldest first */
        std::deque<Addr> recentMisses;

        IndirectPatternDetectorEntry(TagExtractor ext)
          : TaggedEntry(), history(), recentMisses()
        {
            registerTagExtractor(ext);
        }

        void
        invalidate() override
        {
            TaggedEntry::invalidate();
            history.clear();
            recentMisses.clear();
        }
    };
    /** Indirect Pattern Detector (IPD) table */
    AssociativeCache<IndirectPatternDetectorEntry> ipd;

    /**
     * An index-array line whose fill this prefetcher is waiting for
     * (registered while stream-prefetching, when indirectDelta > 0).
     * Fills arrive with physical addresses and, for prefetched lines,
     * no virtual address, so the expected physical address is computed
     * up front from the triggering access's VA/PA pair.
     */
    struct PendingFillLine
    {
        /** Expected physical address of the line */
        Addr linePaddr;
        /** Virtual address of the line */
        Addr lineVaddr;
        /** PC of the index-load stream the line belongs to */
        Addr pc;
        /** Whether the stream is in the secure region */
        bool secure;
    };
    /** Expected-fill table, FIFO-evicted at pendingFillEntries */
    std::list<PendingFillLine> pendingFillLines;

    /** Byte order used to access the cache */
    const ByteOrder byteOrder;

    struct VIMPStats : public statistics::Group
    {
        VIMPStats(statistics::Group *parent);
        /** Chunk payloads sliced into indices */
        statistics::Scalar chunksSliced;
        /** Total index elements extracted from chunk payloads */
        statistics::Scalar indicesExtracted;
        /** Chunk-stream prefetch candidates generated (index array) */
        statistics::Scalar streamCandidates;
        /** Stream candidates suppressed by the per-walk high-water mark */
        statistics::Scalar streamCandidatesSuppressed;
        /** Indirect prefetch candidates generated (target array) */
        statistics::Scalar indirectCandidates;
        /** (baseAddr, shift) patterns detected */
        statistics::Scalar patternsDetected;
        /** Accesses that matched a predicted target of an enabled entry */
        statistics::Scalar confidenceMatches;
        /** IPD entries dropped without finding a pattern */
        statistics::Scalar ipdDropsNoMatch;
        /** Enabled entries demoted after sustained zero confidence */
        statistics::Scalar patternsDemoted;
        /** Index-array lines captured from cache fills (delta mode) */
        statistics::Scalar fillLinesCaptured;
        /** Captured index sets dropped stale or on overflow (delta mode) */
        statistics::Scalar pendingSetsDropped;
    } vimpStats;

    /**
     * Applies a shift value to an index. Negative shifts (as in IMP's
     * shift list) mean an arithmetic right shift.
     */
    static Addr
    applyShift(int64_t index, int shift)
    {
        if (shift >= 0) {
            return static_cast<Addr>(index) << shift;
        } else {
            return static_cast<Addr>(index >> (-shift));
        }
    }

    /**
     * Slice the payload of a chunk access into index values.
     * @param pfi PrefetchInfo of the triggering access
     * @param indices output vector of sign/zero-extended index values
     * @return number of extracted indices (0 if the payload is missing)
     */
    unsigned extractIndices(const PrefetchInfo &pfi,
                            std::vector<int64_t> &indices) const;

    /**
     * Slice a raw byte buffer (a filled cache line of the index array)
     * into index values.
     * @param data pointer to the bytes
     * @param num_bytes number of valid bytes at data
     * @param indices output vector of sign/zero-extended index values
     * @return number of extracted indices
     */
    unsigned extractIndicesRaw(const uint8_t *data, unsigned num_bytes,
                               std::vector<int64_t> &indices) const;

    /**
     * Record that a line of a tracked index array is being prefetched, so
     * its index values can be captured when the fill arrives (delta mode).
     * Deduplicates on the physical address; FIFO-evicts at capacity.
     */
    void registerPendingFillLine(Addr line_vaddr, Addr line_paddr,
                                 Addr pc, bool secure);

    /**
     * Emit indirect prefetches for every buffered index-line set the
     * demand pointer has come within indirectDelta chunks of, and drop
     * sets the demand stream has already passed (delta mode).
     * @param pt_entry the stream's prefetch table entry
     * @param addr virtual address of the current demand chunk
     * @param size size in bytes of the current demand chunk
     * @param addresses candidate list of the current calculatePrefetch call
     */
    void releasePendingTargets(PrefetchTableEntry *pt_entry, Addr addr,
                               unsigned size,
                               std::vector<AddrPriority> &addresses);

    /**
     * Allocate or update an entry in the IPD, pushing the leading indices
     * of an observed chunk into its history
     * @param pt_entry Pointer to the associated prefetch table entry
     * @param indices Index values of the observed chunk
     */
    void allocateOrUpdateIPDEntry(const PrefetchTableEntry *pt_entry,
                                  const std::vector<int64_t> &indices);

    /**
     * Correlate a tracked miss against an IPD entry: search for a recent
     * miss whose address delta from this one equals the shifted delta of
     * two distinct indices of one recent chunk. A match yields
     * (baseAddr, shift) and enables the associated PT entry.
     * @param entry the IPD entry
     * @param miss_addr The address that caused the miss
     */
    void trackMiss(IndirectPatternDetectorEntry *entry, Addr miss_addr);

    /**
     * Checks if an access matches any predicted target of an active PT
     * entry (baseAddr + (index << shift) for any index of its last
     * confidenceChunkHistory chunks — the confirming accesses lag the
     * index reads by several chunks on the OoO core); if so, the
     * indirect confidence counter is incremented.
     * @param addr address of the access
     */
    void checkAccessMatchOnActiveEntries(Addr addr);

    /**
     * Generate one indirect prefetch candidate per index of the chunk,
     * deduplicated at cache-line granularity and capped by
     * maxIndirectTargets.
     */
    void generateIndirectPrefetches(const PrefetchTableEntry *pt_entry,
                                    const std::vector<int64_t> &indices,
                                    std::vector<AddrPriority> &addresses);

  public:
    VectorIndirectMemory(const VectorIndirectMemoryPrefetcherParams &p);
    ~VectorIndirectMemory() = default;

    void calculatePrefetch(const PrefetchInfo &pfi,
                           std::vector<AddrPriority> &addresses,
                           const CacheAccessor &cache) override;

    /**
     * Capture the index values of an expected index-array line when its
     * fill arrives (delta mode). The fill packet carries the whole line;
     * prefetch-generated fills have no virtual address, which is why the
     * expected physical address was registered up front.
     */
    void notifyFill(const CacheAccessProbeArg &acc) override;

    void resetLearnedState() override;
};

} // namespace prefetch
} // namespace gem5

#endif //__MEM_CACHE_PREFETCH_VECTOR_INDIRECT_MEMORY_HH__
