#include "cpu/revela_table.hh"

#include "params/RevelaStreamTable.hh"

namespace gem5
{

RevelaStreamTable::RevelaStreamTable(const RevelaStreamTableParams &p)
    : SimObject(p),
      lruMax(p.lru_max),
      stt(p.stt_entries),
      tableStats(this)
{
    // ROI convention: learned prefetcher state wipes at every stats
    // reset so each region starts with a cold predictor.
    statistics::registerResetCallback([this]() { resetState(); });
}

void
RevelaStreamTable::resetState()
{
    for (auto &e : stt) {
        e = SttEntry();
    }
    avlKnown = false;
    avlElems = 0;
}

void
RevelaStreamTable::notifyVsetvl(uint64_t avl_elems)
{
    avlKnown = true;
    avlElems = avl_elems;
    tableStats.vsetvlSnooped++;
}

void
RevelaStreamTable::notifyVsetvlUnbounded()
{
    avlKnown = false;
    tableStats.vsetvlUnbounded++;
}

unsigned
RevelaStreamTable::numValid() const
{
    unsigned n = 0;
    for (const auto &e : stt) {
        n += e.valid;
    }
    return n;
}

void
RevelaStreamTable::ageOthers(int except)
{
    for (int i = 0; i < (int)stt.size(); i++) {
        if (i == except || !stt[i].valid) {
            continue;
        }
        if (++stt[i].lru >= lruMax) {
            stt[i].valid = false;
            tableStats.staleInvalidations++;
        }
    }
}

void
RevelaStreamTable::notifyVectorAccess(const StaticInst *si, Addr pc,
                                      Addr vaddr)
{
    const StaticInst::VecMemInfo info = si->vecMemInfo();
    if (info.kind != StaticInst::VecMemInfo::UnitStrideLoad &&
        info.kind != StaticInst::VecMemInfo::UnitStrideStore) {
        return;
    }
    // One update per macro-op: micro-op 0 carries the base address,
    // and the granted extent below spans the whole register group.
    if (info.microIdx != 0) {
        return;
    }
    // Without a known RVL the limit address cannot be computed. A
    // granted vl above the shadowed AVL means the shadow is stale
    // (vl = min(AVL, VLMAX) can never exceed the AVL it came from).
    if (!avlKnown || info.vl == 0 || info.elemBytes == 0 ||
        avlElems < info.vl) {
        return;
    }
    const Addr gvlBytes = (Addr)info.vl * info.elemBytes;
    const Addr rvlBytes = (Addr)avlElems * info.elemBytes;
    const Addr limit = vaddr + rvlBytes;

    // STT lookup by the loop-invariant limit address.
    int hit = -1;
    for (int i = 0; i < (int)stt.size(); i++) {
        if (stt[i].valid && stt[i].limitAddr == limit) {
            hit = i;
            break;
        }
    }

    if (hit >= 0) {
        SttEntry &e = stt[hit];
        if (vaddr < e.currentAddr) {
            // Access to a past region of the stream (out-of-order
            // reordering): updating would store outdated data.
            tableStats.backwardAccesses++;
            return;
        }
        e.currentAddr = vaddr + gvlBytes;
        if (e.prefetchedUpTo < e.currentAddr) {
            e.prefetchedUpTo = e.currentAddr;
        }
        e.lru = 0;
        e.lastPc = pc;
        tableStats.sttUpdates++;
        ageOthers(hit);
        if (e.currentAddr >= e.limitAddr) {
            // Final access of the stream.
            e.valid = false;
            tableStats.streamsCompleted++;
        }
        return;
    }

    // Miss: allocate — unless the stream has no prefetchable data
    // beyond this access (RVL <= GVL), where a dead entry would only
    // depress the aggressivity until LRU aging reclaims it.
    if (rvlBytes <= gvlBytes) {
        tableStats.allocationsSkipped++;
        return;
    }
    int victim = -1;
    for (int i = 0; i < (int)stt.size(); i++) {
        if (!stt[i].valid) {
            victim = i;
            break;
        }
        if (victim < 0 || stt[i].lru > stt[victim].lru) {
            victim = i;
        }
    }
    SttEntry &e = stt[victim];
    e.valid = true;
    e.currentAddr = vaddr + gvlBytes;
    e.limitAddr = limit;
    e.prefetchedUpTo = e.currentAddr;
    e.lru = 0;
    e.lastPc = pc;
    tableStats.streamsAllocated++;
    ageOthers(victim);
}

RevelaStreamTable::RevelaTableStats::RevelaTableStats(
    statistics::Group *parent)
    : statistics::Group(parent),
      ADD_STAT(vsetvlSnooped, statistics::units::Count::get(),
               "AVLs snooped at vset{i}vl{i} issue"),
      ADD_STAT(vsetvlUnbounded, statistics::units::Count::get(),
               "vsetvl VLMAX requests (rs1=x0, rd!=x0): shadow "
               "invalidated"),
      ADD_STAT(streamsAllocated, statistics::units::Count::get(),
               "STT entries allocated (new streams)"),
      ADD_STAT(allocationsSkipped, statistics::units::Count::get(),
               "Allocations skipped because RVL <= GVL (no "
               "prefetchable data left)"),
      ADD_STAT(sttUpdates, statistics::units::Count::get(),
               "Forward hits that advanced current@"),
      ADD_STAT(backwardAccesses, statistics::units::Count::get(),
               "Hits below current@ (reordered past-region accesses)"),
      ADD_STAT(streamsCompleted, statistics::units::Count::get(),
               "Streams invalidated by their final access"),
      ADD_STAT(staleInvalidations, statistics::units::Count::get(),
               "Entries invalidated by LRU saturation (stale streams)")
{
}

} // namespace gem5
