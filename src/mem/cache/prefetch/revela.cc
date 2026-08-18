#include "mem/cache/prefetch/revela.hh"

#include <algorithm>

#include "base/logging.hh"
#include "params/RevelaPrefetcher.hh"

namespace gem5
{

namespace prefetch
{

ReVeLA::ReVeLA(const RevelaPrefetcherParams &p)
    : Queued(p),
      tbl(p.stream_table),
      maxPrefetchDistance(p.max_prefetch_distance),
      degree(p.degree),
      initialDistance(p.initial_distance),
      drainEvent([this] { drainTick(); }, name()),
      revelaStats(this)
{
    fatal_if(tbl == nullptr, "%s: no stream_table set. ReVeLA needs "
             "the RevelaStreamTable that is also attached to the CPU's "
             "revela_table param (the config script wires both).",
             name());
    fatal_if(maxPrefetchDistance < 1, "max_prefetch_distance must be "
             ">= 1");
    fatal_if(degree < 1, "degree must be >= 1");
}

unsigned
ReVeLA::aggressivityLimit() const
{
    // The paper's Aggressivity Table (Figure 5): fewer live streams
    // may each run further ahead; 64/32/16/8 lines at the default
    // max_prefetch_distance of 64.
    const unsigned valid = tbl->numValid();
    unsigned limit = maxPrefetchDistance;
    if (valid >= 8) {
        limit = maxPrefetchDistance / 8;
    } else if (valid >= 4) {
        limit = maxPrefetchDistance / 4;
    } else if (valid >= 2) {
        limit = maxPrefetchDistance / 2;
    }
    // The initial-distance floor must stay reachable: a floored
    // stream sits at distance ~initialDistance, and a ceiling below
    // that would throttle it forever. Raising the ceiling to the
    // floor degenerates the stream to a fixed-offset follower.
    return std::max(std::max(limit, initialDistance), 1U);
}

unsigned
ReVeLA::distanceLines(const RevelaStreamTable::SttEntry &e) const
{
    if (e.prefetchedUpTo <= e.currentAddr) {
        return 0;
    }
    return (e.prefetchedUpTo - e.currentAddr + blkSize - 1) / blkSize;
}

void
ReVeLA::applyInitialDistance(RevelaStreamTable::SttEntry &e)
{
    if (initialDistance == 0) {
        return;
    }
    const Addr floor_addr = e.currentAddr
                          + (Addr)initialDistance * blkSize;
    if (e.prefetchedUpTo >= floor_addr) {
        return;
    }
    const Addr jump_to = std::min(floor_addr, e.limitAddr);
    revelaStats.linesSkippedFloor +=
        (blockAddress(jump_to) - blockAddress(e.prefetchedUpTo))
        / blkSize;
    e.prefetchedUpTo = jump_to;
}

bool
ReVeLA::workPending() const
{
    const unsigned limit = aggressivityLimit();
    for (const auto &e : tbl->entries()) {
        if (e.valid && e.prefetchedUpTo < e.limitAddr &&
            distanceLines(e) <= limit) {
            return true;
        }
    }
    return false;
}

void
ReVeLA::emitRound(std::vector<AddrPriority> &addresses)
{
    // Queue-room gate (paper Section 3.3): only trigger while the
    // prefetch queue can hold the requests.
    int room = (int)queueSize - (int)pfq.size()
             - (int)pfqMissingTranslation.size();
    if (room <= 0) {
        revelaStats.drainNoRoom++;
        return;
    }

    const unsigned limit = aggressivityLimit();

    // Paper Figure 7: among the unfinished streams, only those at the
    // minimum prefetched distance emit, and only below the
    // aggressivity cap — the streams advance together.
    unsigned min_dist = 0;
    bool have_min = false;
    for (auto &e : tbl->entries()) {
        if (!e.valid || e.prefetchedUpTo >= e.limitAddr) {
            continue;
        }
        applyInitialDistance(e);
        if (e.prefetchedUpTo >= e.limitAddr) {
            continue; // floored past the extent end
        }
        const unsigned d = distanceLines(e);
        if (d > limit) {
            revelaStats.aggressivityThrottled++;
            continue;
        }
        if (!have_min || d < min_dist) {
            min_dist = d;
            have_min = true;
        }
    }
    if (!have_min) {
        return;
    }

    for (auto &e : tbl->entries()) {
        if (room <= 0) {
            break;
        }
        if (!e.valid || e.prefetchedUpTo >= e.limitAddr ||
            distanceLines(e) != min_dist) {
            continue;
        }
        for (unsigned d = 0; d < degree && room > 0 &&
                 e.prefetchedUpTo < e.limitAddr; d++) {
            const Addr line = blockAddress(e.prefetchedUpTo);
            addresses.push_back(AddrPriority(line, 0));
            e.prefetchedUpTo = line + blkSize;
            room--;
            revelaStats.linesEmitted++;
        }
    }
}

void
ReVeLA::calculatePrefetch(const PrefetchInfo &pfi,
                          std::vector<AddrPriority> &addresses,
                          const CacheAccessor &cache)
{
    // A demand-access event is also a cycle where the trigger runs;
    // the STT itself was already updated on the LSQ side.
    emitRound(addresses);
}

void
ReVeLA::notify(const CacheAccessProbeArg &acc, const PrefetchInfo &pfi)
{
    // Latch this access's translation context for the self-clocked
    // drain: insert() dereferences a VA
    // and a ContextID, and the Request is a shared_ptr that outlives
    // the packet safely.
    if (acc.pkt != nullptr && acc.pkt->req != nullptr &&
        acc.pkt->req->hasVaddr() && acc.pkt->req->hasContextId()) {
        drainCtxReq = acc.pkt->req;
        drainCtxPfi.reset(new PrefetchInfo(pfi, pfi.getAddr()));
        drainCtxCache = &acc.cache;
    }
    Queued::notify(acc, pfi);
    // Start any pending cross-page translations now (see drainTick —
    // this also covers inserts made by this very notify).
    if (!pfqMissingTranslation.empty()) {
        processMissingTranslations(queueSize - pfq.size());
    }
    scheduleDrain();
}

PacketPtr
ReVeLA::getPacket()
{
    // A cache pull frees queue room: re-arm the drain.
    PacketPtr pkt = Queued::getPacket();
    scheduleDrain();
    return pkt;
}

void
ReVeLA::scheduleDrain()
{
    if (drainEvent.scheduled() || !workPending()) {
        return;
    }
    // Every cycle, the paper's fixed trigger cadence (Section 3.3).
    schedule(drainEvent, clockEdge(Cycles(1)));
}

void
ReVeLA::drainTick()
{
    // No context until the first demand access has been observed; the
    // next notify() re-arms.
    if (!drainCtxPfi || !drainCtxCache) {
        return;
    }
    revelaStats.selfDrainTicks++;
    std::vector<AddrPriority> addresses;
    emitRound(addresses);
    if (!addresses.empty()) {
        // A transient packet lends insert() the saved request's VA/PA
        // and context; insert() never retains it (DeferredPacket
        // builds its own packet via createPkt).
        Packet pkt(drainCtxReq, MemCmd::ReadReq);
        PacketPtr pktp = &pkt;
        for (AddrPriority &ap : addresses) {
            ap.first = blockAddress(ap.first);
            const bool same_page = samePage(ap.first,
                                            drainCtxPfi->getAddr());
            if (!same_page) {
                statsQueued.pfSpanPage += 1;
            }
            if (same_page || mmu != nullptr) {
                PrefetchInfo new_pfi(*drainCtxPfi, ap.first);
                statsQueued.pfIdentified++;
                insert(pktp, new_pfi, ap.second, *drainCtxCache);
            }
        }
    }
    // Kick pending cross-page translations. Upstream only starts them
    // from getPacket(), and the cache stops calling getPacket() once
    // pfq is empty (nextPrefetchReadyTime() is MaxTick while every
    // candidate waits in pfqMissingTranslation) — without this kick
    // the drain wedges with all queue room held by untranslated
    // entries, throttling emission to the rare port-idle pull.
    if (!pfqMissingTranslation.empty()) {
        processMissingTranslations(queueSize - pfq.size());
    }
    scheduleDrain();
}

ReVeLA::RevelaStats::RevelaStats(statistics::Group *parent)
    : statistics::Group(parent),
      ADD_STAT(selfDrainTicks, statistics::units::Count::get(),
               "Self-clocked drain firings that evaluated the trigger"),
      ADD_STAT(linesEmitted, statistics::units::Count::get(),
               "Prefetch lines emitted into the queue"),
      ADD_STAT(drainNoRoom, statistics::units::Count::get(),
               "Trigger evaluations skipped for lack of queue room"),
      ADD_STAT(linesSkippedFloor, statistics::units::Count::get(),
               "Stream lines never emitted: the initial-distance "
               "floor jumped the frontier past them"),
      ADD_STAT(aggressivityThrottled, statistics::units::Count::get(),
               "Stream-emissions skipped by the aggressivity cap")
{
}

} // namespace prefetch
} // namespace gem5
