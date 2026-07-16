/**
 * Vector Indirect Memory Prefetcher (VIMP) implementation.
 * See vector_indirect_memory.hh for the design description.
 */

#include "mem/cache/prefetch/vector_indirect_memory.hh"

#include <algorithm>
#include <cstring>

#include "base/logging.hh"
#include "base/trace.hh"
#include "debug/VIMP.hh"
#include "params/VectorIndirectMemoryPrefetcher.hh"
#include "sim/system.hh"

namespace gem5
{

namespace prefetch
{

VectorIndirectMemory::VectorIndirectMemory(
    const VectorIndirectMemoryPrefetcherParams &p)
  : Queued(p),
    shiftValues(p.shift_values),
    prefetchThreshold(p.prefetch_threshold),
    streamCounterThreshold(p.stream_counter_threshold),
    streamingDistance(p.streaming_distance),
    streamDedup(p.stream_dedup),
    indexSize(p.index_size),
    indexSigned(p.index_signed),
    maxIndicesPerChunk(p.max_indices_per_chunk),
    ipdIndicesPerChunk(p.ipd_indices_per_chunk),
    addrArrayLen(p.addr_array_len),
    maxIndirectTargets(p.max_indirect_targets),
    ipdTrainOnHits(p.ipd_train_on_hits),
    ipdChunkHistory(p.ipd_chunk_history),
    confidenceChunkHistory(p.confidence_chunk_history),
    demotionChunks(p.demotion_chunks),
    indirectDelta(p.indirect_delta),
    pendingFillEntries(p.pending_fill_entries),
    pendingIndexSets(p.pending_index_sets),
    prefetchTable((name() + ".PrefetchTable").c_str(),
                  p.pt_table_entries,
                  p.pt_table_assoc,
                  p.pt_table_replacement_policy,
                  p.pt_table_indexing_policy,
                  PrefetchTableEntry(p.num_indirect_counter_bits,
                    genTagExtractor(p.pt_table_indexing_policy))),
    ipd((name() + ".IPD").c_str(), p.ipd_table_entries, p.ipd_table_assoc,
        p.ipd_table_replacement_policy,
        p.ipd_table_indexing_policy,
        IndirectPatternDetectorEntry(
            genTagExtractor(p.ipd_table_indexing_policy))),
    pendingFillLines(),
    byteOrder(p.sys->getGuestByteOrder()),
    vimpStats(this)
{
    fatal_if(indexSize != 1 && indexSize != 2 && indexSize != 4 &&
             indexSize != 8, "index_size must be 1, 2, 4 or 8 bytes");
    fatal_if(ipdIndicesPerChunk == 0, "ipd_indices_per_chunk must be >= 1");
    fatal_if(maxIndicesPerChunk == 0, "max_indices_per_chunk must be >= 1");
    fatal_if(ipdChunkHistory == 0, "ipd_chunk_history must be >= 1");
    fatal_if(confidenceChunkHistory == 0,
             "confidence_chunk_history must be >= 1");
    warn_if(indirectDelta > (unsigned int) streamingDistance,
            "%s: indirect_delta (%u) exceeds streaming_distance (%d); "
            "index lines only fill streaming_distance chunks ahead, so the "
            "effective lookahead is capped there", name(), indirectDelta,
            streamingDistance);
}

void
VectorIndirectMemory::resetLearnedState()
{
    prefetchTable.clear();
    ipd.clear();
    pendingFillLines.clear();
}

VectorIndirectMemory::VIMPStats::VIMPStats(statistics::Group *parent)
  : statistics::Group(parent),
    ADD_STAT(chunksSliced, statistics::units::Count::get(),
        "chunk payloads sliced into index values"),
    ADD_STAT(indicesExtracted, statistics::units::Count::get(),
        "index elements extracted from chunk payloads"),
    ADD_STAT(streamCandidates, statistics::units::Count::get(),
        "chunk-stream prefetch candidates generated (index array)"),
    ADD_STAT(streamCandidatesSuppressed, statistics::units::Count::get(),
        "stream candidates suppressed by the per-walk high-water mark"),
    ADD_STAT(indirectCandidates, statistics::units::Count::get(),
        "indirect prefetch candidates generated (target array)"),
    ADD_STAT(patternsDetected, statistics::units::Count::get(),
        "(baseAddr, shift) patterns detected"),
    ADD_STAT(confidenceMatches, statistics::units::Count::get(),
        "accesses that matched a predicted target of an enabled entry"),
    ADD_STAT(ipdDropsNoMatch, statistics::units::Count::get(),
        "IPD entries dropped without finding a pattern"),
    ADD_STAT(patternsDemoted, statistics::units::Count::get(),
        "enabled entries demoted after sustained zero confidence"),
    ADD_STAT(fillLinesCaptured, statistics::units::Count::get(),
        "index-array lines captured from cache fills (delta mode)"),
    ADD_STAT(pendingSetsDropped, statistics::units::Count::get(),
        "captured index sets dropped stale or on overflow (delta mode)")
{
}

unsigned
VectorIndirectMemory::extractIndices(const PrefetchInfo &pfi,
                                     std::vector<int64_t> &indices) const
{
    indices.clear();
    if (pfi.isWrite() || !pfi.hasData()) {
        return 0;
    }
    unsigned num = std::min(pfi.getSize() / indexSize, maxIndicesPerChunk);
    for (unsigned i = 0; i < num; i++) {
        int64_t value = 0;
        switch (indexSize) {
          case sizeof(uint8_t):
            value = indexSigned ?
                (int64_t)(int8_t) pfi.get<uint8_t>(byteOrder, i) :
                (int64_t) pfi.get<uint8_t>(byteOrder, i);
            break;
          case sizeof(uint16_t):
            value = indexSigned ?
                (int64_t)(int16_t) pfi.get<uint16_t>(byteOrder, i) :
                (int64_t) pfi.get<uint16_t>(byteOrder, i);
            break;
          case sizeof(uint32_t):
            value = indexSigned ?
                (int64_t)(int32_t) pfi.get<uint32_t>(byteOrder, i) :
                (int64_t) pfi.get<uint32_t>(byteOrder, i);
            break;
          case sizeof(uint64_t):
            value = (int64_t) pfi.get<uint64_t>(byteOrder, i);
            break;
        }
        indices.push_back(value);
    }
    return indices.size();
}

unsigned
VectorIndirectMemory::extractIndicesRaw(const uint8_t *data,
        unsigned num_bytes, std::vector<int64_t> &indices) const
{
    indices.clear();
    unsigned num = std::min(num_bytes / indexSize, maxIndicesPerChunk);
    for (unsigned i = 0; i < num; i++) {
        int64_t value = 0;
        switch (indexSize) {
          case sizeof(uint8_t): {
            uint8_t v = data[i];
            value = indexSigned ? (int64_t)(int8_t) v : (int64_t) v;
            break;
          }
          case sizeof(uint16_t): {
            uint16_t v;
            std::memcpy(&v, data + i * sizeof(v), sizeof(v));
            v = (byteOrder == ByteOrder::big) ? betoh(v) : letoh(v);
            value = indexSigned ? (int64_t)(int16_t) v : (int64_t) v;
            break;
          }
          case sizeof(uint32_t): {
            uint32_t v;
            std::memcpy(&v, data + i * sizeof(v), sizeof(v));
            v = (byteOrder == ByteOrder::big) ? betoh(v) : letoh(v);
            value = indexSigned ? (int64_t)(int32_t) v : (int64_t) v;
            break;
          }
          case sizeof(uint64_t): {
            uint64_t v;
            std::memcpy(&v, data + i * sizeof(v), sizeof(v));
            v = (byteOrder == ByteOrder::big) ? betoh(v) : letoh(v);
            value = (int64_t) v;
            break;
          }
        }
        indices.push_back(value);
    }
    return indices.size();
}

void
VectorIndirectMemory::calculatePrefetch(const PrefetchInfo &pfi,
    std::vector<AddrPriority> &addresses,
    const CacheAccessor &cache)
{
    // This prefetcher requires a PC
    if (!pfi.hasPC()) {
        return;
    }

    bool is_secure = pfi.isSecure();
    Addr pc = pfi.getPC();
    Addr addr = pfi.getAddr();
    unsigned size = pfi.getSize();
    bool miss = pfi.isCacheMiss();

    checkAccessMatchOnActiveEntries(addr);

    // Feed element-sized accesses (potential indirect targets, at most 8
    // bytes each) to every valid IPD entry. Each entry correlates
    // independently, so several concurrently-training streams cannot
    // steal each other's misses. Wider accesses are further index
    // chunks: they skip this and take the table path below.
    if ((miss || ipdTrainOnHits) && size <= 8) {
        bool tracked = false;
        for (auto &ipd_entry : ipd) {
            if (!ipd_entry.isValid()) {
                continue;
            }
            trackMiss(&ipd_entry, addr);
            tracked = true;
        }
        if (tracked) {
            return;
        }
    }

    const PrefetchTableEntry::KeyType key{pc, is_secure};
    PrefetchTableEntry *pt_entry = prefetchTable.findEntry(key);
    if (pt_entry == nullptr) {
        pt_entry = prefetchTable.findVictim(key);
        assert(pt_entry != nullptr);
        prefetchTable.insertEntry(key, pt_entry);
        pt_entry->address = addr;
        pt_entry->lastSize = size;
        pt_entry->secure = is_secure;
        return;
    }

    prefetchTable.accessEntry(pt_entry);

    if (pt_entry->address == addr) {
        // Repeated access to the same chunk; nothing new to learn.
        return;
    }

    // Chunk-granular stream detection: a streaming access starts exactly
    // where the previous request ended, whatever the chunk size is. This
    // replaces IMP's element-granular (+/- a few bytes) stride check.
    if (addr == pt_entry->address + pt_entry->lastSize) {
        pt_entry->streamCounter += 1;
    } else {
        // Discontinuity (e.g. the kernel restarted the walk). Re-train
        // the stream but keep the learned indirect pattern. Captured
        // index sets belong to the abandoned walk, so drop them, and
        // reset the high-water mark so the new walk re-prefetches.
        pt_entry->streamCounter = 0;
        pt_entry->streamIssuedUpTo = 0;
        vimpStats.pendingSetsDropped += pt_entry->pendingChunks.size();
        pt_entry->pendingChunks.clear();
    }
    pt_entry->address = addr;
    pt_entry->lastSize = size;
    pt_entry->secure = is_secure;

    bool streaming = pt_entry->streamCounter >=
        (unsigned int) streamCounterThreshold;

    if (streaming) {
        // Stream-prefetch the index array streamingDistance chunks ahead.
        // A chunk can span several cache lines (VLEN*LMUL/8 larger than
        // the line size), so emit every line the future chunks cover.
        Addr from = addr + size;
        Addr to = addr + size * (Addr)(streamingDistance + 1);
        Addr line = blockAddress(from);
        for (; line < to; line += blkSize) {
            // The window advances one chunk per access but spans
            // streamingDistance chunks, so consecutive accesses re-visit
            // almost every line. Emit each line once per walk: re-emitted
            // duplicates only get dropped by the queue filter or, worse,
            // issue anyway and are miscounted as late (in-cache hits).
            if (!streamDedup || line >= pt_entry->streamIssuedUpTo) {
                addresses.push_back(AddrPriority(line, 0));
                vimpStats.streamCandidates++;
            } else {
                vimpStats.streamCandidatesSuppressed++;
            }

            // Delta mode: remember where these index lines will land so
            // their values can be captured at fill time. The fill arrives
            // with (only) a physical address; on the same page the PA
            // offset equals the VA offset, past a page boundary the frame
            // is unknown, so those lines are skipped (their chunks fall
            // back to demand-time coverage). Lines already in the cache
            // have no fill coming. This sweep deliberately ignores the
            // high-water mark: registration is gated on pt_entry->enabled,
            // which may become true after a line was first emitted.
            if (indirectDelta > 0 && pt_entry->enabled && line > addr &&
                samePage(line, addr)) {
                Addr line_pa = pfi.getPaddr() + (line - addr);
                if (!cache.inCache(line_pa, is_secure)) {
                    registerPendingFillLine(line, line_pa, pc, is_secure);
                }
            }
        }
        if (streamDedup && line > pt_entry->streamIssuedUpTo) {
            // First un-emitted line of this walk (loop exits at the first
            // line-aligned address past the window).
            pt_entry->streamIssuedUpTo = line;
        }
    }

    // Slice the chunk payload into index values. Only possible when the
    // access carried its data (i.e. it hit in this cache); misses still
    // train the stream detector above.
    std::vector<int64_t> indices;
    if (extractIndices(pfi, indices) == 0) {
        return;
    }
    vimpStats.chunksSliced++;
    vimpStats.indicesExtracted += indices.size();

    if (!pt_entry->enabled) {
        // Only correlate confirmed chunk streams; random-access PCs (e.g.
        // the gather element accesses themselves) never reach the IPD.
        if (streaming) {
            allocateOrUpdateIPDEntry(pt_entry, indices);
        }
    } else {
        // Enabled entry: push the new chunk's indices into the confidence
        // window and update the bookkeeping once per chunk. The window
        // must cover the index-read-to-gather-miss lag, or the confirming
        // accesses would only ever be checked against newer, disjoint
        // chunks and confidence could never build.
        pt_entry->recentIndices.push_back(indices);
        if (pt_entry->recentIndices.size() > confidenceChunkHistory) {
            pt_entry->recentIndices.pop_front();
        }
        if (!pt_entry->increasedIndirectCounter) {
            pt_entry->indirectCounter--;
            // A falsely learned (baseAddr, shift) predicts nothing and
            // would otherwise block this stream from ever retraining;
            // demote it after a sustained stretch at zero confidence.
            if (demotionChunks > 0 && pt_entry->indirectCounter == 0) {
                pt_entry->noMatchChunks += 1;
                if (pt_entry->noMatchChunks >= demotionChunks) {
                    DPRINTF(VIMP, "PT %#x DEMOTED: baseAddr=%#x shift=%d "
                            "matched nothing for %u chunks, retraining\n",
                            (Addr) pt_entry, pt_entry->baseAddr,
                            pt_entry->shift, pt_entry->noMatchChunks);
                    pt_entry->enabled = false;
                    pt_entry->noMatchChunks = 0;
                    pt_entry->recentIndices.clear();
                    vimpStats.patternsDemoted++;
                    return;
                }
            }
        } else {
            // Cleared here to check whether the new chunk's predicted
            // targets get any matching access.
            pt_entry->increasedIndirectCounter = false;
            pt_entry->noMatchChunks = 0;
        }

        if (pt_entry->indirectCounter > prefetchThreshold) {
            if (indirectDelta == 0) {
                // Immediate mode: targets of the current chunk, straight
                // from its own payload.
                generateIndirectPrefetches(pt_entry, indices, addresses);
            } else {
                // Delta mode: targets of the chunks up to indirectDelta
                // ahead, from index lines captured at fill time.
                releasePendingTargets(pt_entry, addr, size, addresses);
            }
        }
    }
}

void
VectorIndirectMemory::generateIndirectPrefetches(
    const PrefetchTableEntry *pt_entry,
    const std::vector<int64_t> &indices,
    std::vector<AddrPriority> &addresses)
{
    // Gather indices frequently share cache lines; deduplicate the
    // candidates at line granularity.
    std::vector<Addr> lines;
    for (int64_t index : indices) {
        if (lines.size() >= maxIndirectTargets) {
            break;
        }
        Addr pf_addr = pt_entry->baseAddr +
            applyShift(index, pt_entry->shift);
        Addr line = blockAddress(pf_addr);
        if (std::find(lines.begin(), lines.end(), line) != lines.end()) {
            continue;
        }
        lines.push_back(line);
        addresses.push_back(AddrPriority(pf_addr, 0));
        DPRINTF(VIMP, "PT %#x indirect target: idx=%#x -> addr=%#x "
                "(base=%#x shift=%d)\n", (Addr) pt_entry, index, pf_addr,
                pt_entry->baseAddr, pt_entry->shift);
    }
    vimpStats.indirectCandidates += lines.size();
}

void
VectorIndirectMemory::registerPendingFillLine(Addr line_vaddr,
        Addr line_paddr, Addr pc, bool secure)
{
    for (const PendingFillLine &p : pendingFillLines) {
        if (p.linePaddr == line_paddr && p.secure == secure) {
            return; // already expected
        }
    }
    if (pendingFillLines.size() >= pendingFillEntries) {
        pendingFillLines.pop_front();
    }
    pendingFillLines.push_back({line_paddr, line_vaddr, pc, secure});
    DPRINTF(VIMP, "delta: expecting fill of index line VA %#x (PA %#x) "
            "for PC %#x\n", line_vaddr, line_paddr, pc);
}

void
VectorIndirectMemory::notifyFill(const CacheAccessProbeArg &acc)
{
    if (indirectDelta == 0 || pendingFillLines.empty()) {
        return;
    }
    const PacketPtr pkt = acc.pkt;
    // Upgrade and whole-line-write fills carry no payload.
    if (!pkt->hasData()) {
        return;
    }

    Addr line_pa = blockAddress(pkt->getAddr());
    auto it = pendingFillLines.begin();
    while (it != pendingFillLines.end() &&
           !(it->linePaddr == line_pa && it->secure == pkt->isSecure())) {
        ++it;
    }
    if (it == pendingFillLines.end()) {
        return;
    }
    const PendingFillLine rec = *it;
    pendingFillLines.erase(it);

    // Re-look the stream up by PC; the entry may have been evicted or
    // re-trained since the line was registered.
    const PrefetchTableEntry::KeyType key{rec.pc, rec.secure};
    PrefetchTableEntry *pt_entry = prefetchTable.findEntry(key);
    if (pt_entry == nullptr || !pt_entry->enabled) {
        return;
    }

    std::vector<int64_t> indices;
    if (extractIndicesRaw(pkt->getConstPtr<uint8_t>(), pkt->getSize(),
                          indices) == 0) {
        return;
    }
    if (pt_entry->pendingChunks.size() >= pendingIndexSets) {
        pt_entry->pendingChunks.pop_front();
        vimpStats.pendingSetsDropped++;
    }
    DPRINTF(VIMP, "delta: captured %d indices from fill of index line "
            "VA %#x (PC %#x)\n", (int) indices.size(), rec.lineVaddr,
            rec.pc);
    pt_entry->pendingChunks.push_back({rec.lineVaddr, std::move(indices)});
    vimpStats.fillLinesCaptured++;
}

void
VectorIndirectMemory::releasePendingTargets(PrefetchTableEntry *pt_entry,
        Addr addr, unsigned size, std::vector<AddrPriority> &addresses)
{
    // Fills can complete out of order, so scan the whole buffer rather
    // than assuming address order.
    const Addr horizon = addr + (Addr) indirectDelta * size;
    auto it = pt_entry->pendingChunks.begin();
    while (it != pt_entry->pendingChunks.end()) {
        if (it->lineVaddr < addr) {
            // The demand stream already passed this line (e.g. captured
            // just before a restart); its targets would be late.
            vimpStats.pendingSetsDropped++;
            it = pt_entry->pendingChunks.erase(it);
        } else if (it->lineVaddr <= horizon) {
            DPRINTF(VIMP, "delta: releasing targets of index line VA %#x "
                    "at demand chunk VA %#x\n", it->lineVaddr, addr);
            generateIndirectPrefetches(pt_entry, it->indices, addresses);
            it = pt_entry->pendingChunks.erase(it);
        } else {
            ++it;
        }
    }
}

void
VectorIndirectMemory::allocateOrUpdateIPDEntry(
    const PrefetchTableEntry *pt_entry, const std::vector<int64_t> &indices)
{
    // The address of the pt_entry is used to index the IPD
    Addr ipd_entry_addr = (Addr) pt_entry;
    const IndirectPatternDetectorEntry::KeyType key{ipd_entry_addr, false};
    const size_t num_used =
        std::min(indices.size(), (size_t) ipdIndicesPerChunk);
    IndirectPatternDetectorEntry *ipd_entry = ipd.findEntry(key);
    if (ipd_entry == nullptr) {
        ipd_entry = ipd.findVictim(key);
        assert(ipd_entry != nullptr);
        if (ipd_entry->isValid()) {
            DPRINTF(VIMP, "IPD: PT %#x DROPPED: evicted for PT %#x\n",
                    ipd_entry->getTag(), ipd_entry_addr);
            vimpStats.ipdDropsNoMatch++;
            ipd.invalidate(ipd_entry);
        }
        ipd.insertEntry(key, ipd_entry);
        DPRINTF(VIMP, "IPD: PT %#x allocated, tracking misses\n",
                ipd_entry_addr);
    } else {
        ipd.accessEntry(ipd_entry);
    }

    // Push this chunk's leading indices into the history. Misses are
    // correlated against every remembered chunk, so index reads racing
    // ahead of their gathers' (MSHR-throttled) misses on the OoO core
    // are harmless while the lag stays within ipdChunkHistory chunks.
    ipd_entry->history.emplace_back(indices.begin(),
                                    indices.begin() + num_used);
    if (ipd_entry->history.size() > (size_t) ipdChunkHistory) {
        ipd_entry->history.pop_front();
    }
}

void
VectorIndirectMemory::trackMiss(IndirectPatternDetectorEntry *entry,
                                Addr miss_addr)
{
    // Ignore re-notifications of an address already recorded (e.g.
    // further accesses coalescing on an outstanding line).
    for (Addr old_miss : entry->recentMisses) {
        if (old_miss == miss_addr) {
            return;
        }
    }

    // Two misses of the same gather satisfy
    //     new_miss - old_miss == (idx_a << s) - (idx_b << s)
    // for two distinct indices of the single chunk that produced them —
    // an invariant that needs no alignment between chunk reads and miss
    // arrival. Search recent misses x shifts x remembered chunks for
    // such a pair; it also pins down the base:
    //     baseAddr = new_miss - (idx_a << s).
    for (Addr old_miss : entry->recentMisses) {
        const Addr delta = miss_addr - old_miss;
        for (int s = 0; s < (int) shiftValues.size(); s++) {
            const int shift = shiftValues[s];
            // A left shift by s can only produce deltas with s low zero
            // bits; skip the pair search otherwise.
            if (shift > 0 && (delta & ((1ULL << shift) - 1))) {
                continue;
            }
            for (const std::vector<int64_t> &chunk : entry->history) {
                for (int64_t idx_a : chunk) {
                    const Addr shifted_a = applyShift(idx_a, shift);
                    for (int64_t idx_b : chunk) {
                        if (idx_a == idx_b ||
                            delta != shifted_a - applyShift(idx_b, shift)) {
                            continue;
                        }
                        // Match found! Fill the corresponding pt_entry
                        PrefetchTableEntry *pt_entry =
                            (PrefetchTableEntry *) entry->getTag();
                        pt_entry->baseAddr = miss_addr - shifted_a;
                        pt_entry->shift = shift;
                        pt_entry->enabled = true;
                        pt_entry->indirectCounter.reset();
                        pt_entry->increasedIndirectCounter = false;
                        pt_entry->noMatchChunks = 0;
                        // Seed the confidence window with the detector's
                        // chunk history: the misses arriving next belong
                        // to chunks read before detection, and an empty
                        // window would leave them unmatchable.
                        pt_entry->recentIndices.assign(
                            entry->history.begin(), entry->history.end());
                        while (pt_entry->recentIndices.size() >
                               confidenceChunkHistory) {
                            pt_entry->recentIndices.pop_front();
                        }
                        vimpStats.patternsDetected++;
                        DPRINTF(VIMP, "IPD: PT %#x pattern DETECTED: "
                                "baseAddr=%#x shift=%d (miss %#x vs %#x, "
                                "idx %#x vs %#x), entry released\n",
                                entry->getTag(), pt_entry->baseAddr,
                                pt_entry->shift, miss_addr, old_miss,
                                idx_a, idx_b);
                        // Release the IPD entry
                        ipd.invalidate(entry);
                        return;
                    }
                }
            }
        }
    }

    // No pattern (yet): remember this miss for pairing with later ones
    entry->recentMisses.push_back(miss_addr);
    if (entry->recentMisses.size() > (size_t) addrArrayLen) {
        entry->recentMisses.pop_front();
    }
}

void
VectorIndirectMemory::checkAccessMatchOnActiveEntries(Addr addr)
{
    for (auto &pt_entry : prefetchTable) {
        if (!pt_entry.enabled) {
            continue;
        }
        bool matched = false;
        for (const std::vector<int64_t> &chunk : pt_entry.recentIndices) {
            for (int64_t index : chunk) {
                if (addr == pt_entry.baseAddr +
                            applyShift(index, pt_entry.shift)) {
                    pt_entry.indirectCounter++;
                    pt_entry.increasedIndirectCounter = true;
                    vimpStats.confidenceMatches++;
                    matched = true;
                    break;
                }
            }
            if (matched) {
                break;
            }
        }
    }
}

} // namespace prefetch
} // namespace gem5
