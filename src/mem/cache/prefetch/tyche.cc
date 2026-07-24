/**
 * Tyche prefetcher (cache side) implementation. See tyche.hh for the
 * design and cpu/tyche_table.hh for the CPU-side half.
 */

#include "mem/cache/prefetch/tyche.hh"

#include <cstring>

#include "base/logging.hh"
#include "base/trace.hh"
#include "debug/Tyche.hh"
#include "params/TychePrefetcher.hh"
#include "sim/byteswap.hh"

namespace gem5
{

namespace prefetch
{

Tyche::Tyche(const TychePrefetcherParams &p)
  : Queued(p),
    tct(p.chain_table),
    strideDistance(p.stride_distance),
    strideOnly(p.stride_only),
    walkEntries(p.walk_entries),
    successorsPerWakeup(p.successors_per_wakeup),
    maxChainHops(p.max_chain_hops),
    pendingTargetEntries(p.pending_target_entries),
    walks(),
    pendingTargets(),
    tycheStats(this)
{
    fatal_if(tct == nullptr, "%s: no chain_table set. Tyche needs the "
             "TycheChainTable that is also attached to the CPU's "
             "tyche_table param (the config script wires both).", name());
    fatal_if(strideDistance == 0, "stride_distance must be >= 1");
    fatal_if(walkEntries == 0, "walk_entries must be >= 1");
    fatal_if(successorsPerWakeup == 0, "successors_per_wakeup must be >= 1");
}

void
Tyche::resetLearnedState()
{
    // The TycheChainTable registers its own reset callback; only this
    // prefetcher's runtime state is cleared here.
    walks.clear();
    pendingTargets.clear();
}

Tyche::TychePfStats::TychePfStats(statistics::Group *parent)
  : statistics::Group(parent),
    ADD_STAT(strideCandidates, statistics::units::Count::get(),
        "IP-stride prefetch candidates emitted"),
    ADD_STAT(walksStarted, statistics::units::Count::get(),
        "walks started at a chain head"),
    ADD_STAT(walksDroppedFull, statistics::units::Count::get(),
        "oldest walk steps evicted because the walk list was full"),
    ADD_STAT(walksIssued, statistics::units::Count::get(),
        "walk steps whose prefetch was observed issuing (VA->PA)"),
    ADD_STAT(fillsCaptured, statistics::units::Count::get(),
        "walk-step fills captured and replayed"),
    ADD_STAT(aluReplays, statistics::units::Count::get(),
        "ALU link replays executed"),
    ADD_STAT(chainCandidates, statistics::units::Count::get(),
        "chain prefetch candidates generated (dependent loads)"),
    ADD_STAT(chainTargetsFiltered, statistics::units::Count::get(),
        "chain targets dropped by the canonical-address filter"),
    ADD_STAT(pendingFlushed, statistics::units::Count::get(),
        "fill-computed targets flushed into the candidate list"),
    ADD_STAT(walksStale, statistics::units::Count::get(),
        "walk steps discarded after a chain-table clear")
{
}

void
Tyche::registerWalk(Addr target_va, int dct_ptr, bool secure)
{
    if (walks.size() >= walkEntries) {
        // Evict the oldest step (the artifact AGQ's pop_when_full
        // mode). Drop-new would wedge permanently: entries whose
        // prefetch never issues (line already cached, queue squash)
        // have no other exit path here, unlike the artifact where every
        // prefetch reliably produces a fill callback.
        walks.pop_front();
        tycheStats.walksDroppedFull++; // artifact AGQ_FULL
    }
    WalkEntry w;
    w.targetVa = target_va;
    w.lineVa = blockAddress(target_va);
    w.dctPtr = dct_ptr;
    w.gen = tct->generation();
    w.secure = secure;
    walks.push_back(w);
    DPRINTF(Tyche, "walk step registered: target VA %#x link %d\n",
            target_va, dct_ptr);
}

void
Tyche::calculatePrefetch(const PrefetchInfo &pfi,
    std::vector<AddrPriority> &addresses,
    const CacheAccessor &cache)
{
    // Flush fill-computed chain targets first: they are demand-imminent
    // and this access supplies the translation context fills lack (same
    // pattern as GDP).
    for (Addr t : pendingTargets) {
        addresses.push_back(AddrPriority(t, 0));
        tycheStats.pendingFlushed++;
    }
    pendingTargets.clear();

    if (pfi.isWrite() || !pfi.hasPC()) {
        return; // the IPT is trained on loads only
    }

    const Addr addr = pfi.getAddr();
    const Addr pc = pfi.getPC();

    const TycheChainTable::StrideTrigger trig = tct->observeAccess(pc, addr);
    if (!trig.trigger) {
        return;
    }

    // IP-stride prefetch, stride_distance iterations ahead (artifact
    // tyche.cc:266-273).
    const Addr target = addr + trig.stride * (int64_t)strideDistance;
    addresses.push_back(AddrPriority(target, 0));
    tycheStats.strideCandidates++;

    // If this load roots a replayable chain, the stride target starts a
    // walk: the fill of that line carries the head-load value the chain
    // replays on (artifact search_ima -> AGQ insert).
    if (!strideOnly) {
        const int head = tct->chainTrigger(pc);
        if (head >= 0) {
            registerWalk(target, head, pfi.isSecure());
            tycheStats.walksStarted++;
        }
    }
}

PacketPtr
Tyche::getPacket()
{
    // A prefetch is about to issue. The issued packet's request is
    // PA-only (createPkt, queued.cc), so peek the DeferredPacket
    // BEFORE delegating: its PrefetchInfo still holds the VA the
    // candidate was generated with, and its pkt the translated PA.
    // Every walk step waiting on this VA line (multiple triggers can
    // predict elements of one line) learns the PA that notifyFill()
    // will see.
    if (!pfq.empty() && pfq.front().pkt != nullptr && !walks.empty()) {
        const DeferredPacket &dp = pfq.front();
        const Addr line_va = blockAddress(dp.pfInfo.getAddr());
        const Addr line_pa = blockAddress(dp.pkt->getAddr());
        const bool secure = dp.pfInfo.isSecure();
        for (WalkEntry &w : walks) {
            if (!w.awaitingFill && w.lineVa == line_va &&
                w.secure == secure) {
                w.linePa = line_pa;
                w.awaitingFill = true;
                tycheStats.walksIssued++;
            }
        }
    }
    return Queued::getPacket();
}

void
Tyche::chase(int dct_ptr, uint64_t value, bool secure)
{
    // Replay the chain below one load link on its loaded value. The
    // artifact spreads this over cycle_operate cycles (2-3 per ALU op);
    // here it runs synchronously at fill time, bounded by maxChainHops
    // (which also guards against back-pointer cycles).
    std::vector<std::pair<int, uint64_t>> worklist;
    worklist.emplace_back(dct_ptr, value);
    unsigned hops = 0;
    while (!worklist.empty() && hops < maxChainHops) {
        const auto [ptr, v] = worklist.back();
        worklist.pop_back();
        hops++;
        for (int s : tct->successors(ptr, successorsPerWakeup)) {
            const TycheChainTable::LinkInfo li = tct->link(s);
            if (!li.valid) {
                continue;
            }
            if (li.isLoad) {
                // pf_address = constant (imm offset / trained base) +
                // propagated value (artifact tyche.cc:99).
                const Addr target = li.src + v;
                if (target == 0 || (target & 0xffffff0000000000ULL)) {
                    tycheStats.chainTargetsFiltered++;
                    continue;
                }
                if (pendingTargets.size() < pendingTargetEntries) {
                    pendingTargets.push_back(target);
                    tycheStats.chainCandidates++;
                }
                if (tct->hasSuccessor(s)) {
                    // The chain continues past this load: multi-hop.
                    registerWalk(target, s, secure);
                }
            } else {
                tycheStats.aluReplays++;
                worklist.emplace_back(s, tct->executeAlu(s, v));
            }
        }
    }
}

void
Tyche::notifyFill(const CacheAccessProbeArg &acc)
{
    if (walks.empty()) {
        return;
    }
    const PacketPtr pkt = acc.pkt;
    if (!pkt->hasData()) {
        return; // upgrades and whole-line writes carry no payload
    }
    const Addr line_pa = blockAddress(pkt->getAddr());

    // Process every walk step waiting on this line (the artifact loops
    // idm_load_return over all matching AGQ items).
    auto it = walks.begin();
    while (it != walks.end()) {
        if (!it->awaitingFill || it->linePa != line_pa ||
            it->secure != pkt->isSecure()) {
            ++it;
            continue;
        }
        const WalkEntry w = *it;
        it = walks.erase(it);

        if (w.gen != tct->generation()) {
            tycheStats.walksStale++; // links cleared under this walk
            continue;
        }
        const TycheChainTable::LinkInfo li = tct->link(w.dctPtr);
        if (!li.valid || !li.isLoad || li.loadSize == 0) {
            tycheStats.walksStale++;
            continue;
        }
        const unsigned offset = w.targetVa & (blkSize - 1);
        if (offset + li.loadSize > blkSize ||
            pkt->getSize() < blkSize) {
            continue; // element straddles the line: cannot extract
        }

        // Read the loaded value from the fill payload (the artifact
        // reads its shadow memory in idm_load_return; the real fill
        // data is better).
        uint64_t raw = 0;
        std::memcpy(&raw, pkt->getConstPtr<uint8_t>() + offset,
                    li.loadSize);
        uint64_t value = letoh(raw);
        if (!li.loadUnsigned) {
            const unsigned shift = 64 - 8 * li.loadSize;
            value = (uint64_t)((int64_t)(value << shift) >> shift);
        } else if (li.loadSize < 8) {
            value &= (1ULL << (8 * li.loadSize)) - 1;
        }
        tycheStats.fillsCaptured++;
        DPRINTF(Tyche, "fill captured: VA %#x link %d value %#x\n",
                w.targetVa, w.dctPtr, value);

        chase(w.dctPtr, value, w.secure);
    }
}

} // namespace prefetch
} // namespace gem5
