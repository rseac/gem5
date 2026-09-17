/**
 * ViperRtl implementation. See viper_rtl.hh and the viper_rtl README.
 */
#include "mem/cache/prefetch/viper_rtl.hh"

#include <cstring>

#include "base/logging.hh"
#include "base/trace.hh"
#include "debug/ViperRtl.hh"
#include "mem/request.hh"
#include "params/ViperRtlPrefetcher.hh"
#include "sim/system.hh"

namespace gem5
{

namespace prefetch
{

ViperRtl::ViperRtl(const ViperRtlPrefetcherParams &p)
  : Base(p),
    drainEvent([this] { drainTick(); }, name()),
    tbl(p.link_table),
    announceTbl(p.stream_table),
    limitGate(p.limit_gate),
    captureOnReadHit(p.capture_on_read_hit),
    latency(p.latency),
    tagPrefetch(p.tag_prefetch),
    traceFileName(p.trace_file),
    rtlStats(this)
{
    fatal_if(tbl == nullptr, "%s: no link_table set (the config script "
             "wires the CPU's VectorChainTable)", name());
    fatal_if(!tbl->foldedFormsEnabled(), "%s: the VectorChainTable must "
             "have folded_forms on", name());
    fatal_if(limitGate && announceTbl == nullptr, "%s: limit_gate needs "
             "the RevelaStreamTable sideband (stream_table)", name());
    fatal_if(!useVirtualAddresses, "%s: the RTL trains on virtual "
             "addresses (use_virtual_addresses must stay true)", name());
    fatal_if(p.throttle_control_percentage != 0, "%s: "
             "throttle_control_percentage is not implemented in the RTL "
             "port", name());

    cfg.prefetch_distance = p.prefetch_distance;
    cfg.vlen = p.vlen;
    cfg.stream_only = p.stream_only;
    cfg.index_line_queue_entries = p.index_line_queue_entries;
    cfg.producer_slots = p.producer_slots;
    cfg.routing_entries = p.routing_entries;
    cfg.capture_on_read_hit = p.capture_on_read_hit;
    cfg.retired_tail_entries = p.retired_tail_entries;
    cfg.capture_read_hit_own_producer = p.capture_read_hit_own_producer;
    cfg.capture_queue_gate = p.capture_queue_gate;
    cfg.drop_batch_confidence = p.drop_batch_confidence;
    fatal_if(p.drop_batch_confidence != 0 &&
             (p.drop_batch_fraction <= 0.0 || p.drop_batch_fraction > 1.0),
             "drop_batch_fraction must be in (0, 1]");
    cfg.drop_batch_fraction_q16 =
        (unsigned)(p.drop_batch_fraction * 65536.0 + 0.5);
    fatal_if(p.drain_floor < 0, "drain_floor must be >= 0");
    cfg.drain_floor = p.drain_floor;
    cfg.dedup_buffer_size = p.dedup_buffer_size;
    cfg.evict_dedup_entries = p.evict_dedup_entries;
    cfg.conversion_lanes = p.conversion_lanes;
    cfg.drop_on_full = p.drop_on_full;
    cfg.pre_lane_dedup = p.pre_lane_dedup;
    cfg.consumers_per_producer = p.consumers_per_producer;
    cfg.random_drop_interval = p.random_drop_interval;
    cfg.random_drop_rates_q8.clear();
    for (double r : p.random_drop_rates) {
        fatal_if(r < 0.0 || r > 1.0, "random_drop_rates entries must be "
                 "in [0, 1]");
        cfg.random_drop_rates_q8.push_back((unsigned)(r * 256.0 + 0.5));
    }
    fatal_if(p.random_drop_interval != 0 && p.random_drop_rates.empty(),
             "random_drop_rates must be non-empty when random_drop_interval "
             "is set");
    cfg.random_drop_geometric = p.random_drop_geometric;
    cfg.random_drop_min_interval = p.random_drop_min_interval;
    cfg.random_drop_seed = p.random_drop_seed;
    cfg.limit_gate = p.limit_gate;
    cfg.queue_size = p.queue_size;
    cfg.queue_squash = p.queue_squash;
    cfg.queue_filter = p.queue_filter;
    cfg.cache_snoop = p.cache_snoop;
    cfg.has_mmu = false;          // updated once the MMU is registered
    cfg.page_shift = floorLog2(pageBytes);

    // The MMU is registered after construction (addMMU); the RTL only
    // needs to know whether page crossing is allowed, which the config
    // script decides by registering one. Defer the driver to the first
    // use so the flag is right.
    rtl = nullptr;
    tbl->attachFormSink(this);
}

ViperRtl::~ViperRtl()
{
    delete rtl;
}

// Lazily build the driver: the MMU pointer is only known after init.
#define RTL_READY()                                                        \
    do {                                                                    \
        if (rtl == nullptr) {                                               \
            cfg.has_mmu = (mmu != nullptr);                                 \
            rtl = new viper_rtl::Driver(cfg, *this, traceFileName);         \
            const std::string err = viper_rtl::Driver::checkConfig(         \
                cfg, rtl->capacities());                                    \
            fatal_if(!err.empty(), "%s: %s", name(), err.c_str());          \
            slots.resize(rtl->capacities().queue_entries);                  \
        }                                                                   \
    } while (0)

// ---------------------------------------------------------------------
// Stats
// ---------------------------------------------------------------------
ViperRtl::ViperRtlStats::ViperRtlStats(statistics::Group *parent)
  : statistics::Group(parent),
    ADD_STAT(rtlCycles, statistics::units::Count::get(),
        "RTL clock cycles spent inside prefetcher events"),
    ADD_STAT(rtlEvents, statistics::units::Count::get(),
        "prefetcher events presented to the RTL"),
    ADD_STAT(rtlMaxCyclesPerEvent, statistics::units::Count::get(),
        "largest RTL cycle count of one event")
{
    for (int k = 0; k < viper_rtl::ST_NUM; k++) {
        auto *s = new statistics::Scalar(this, viper_rtl::statNames[k],
                                         statistics::units::Count::get(),
                                         viper_rtl::statNames[k]);
        byIndex.push_back(s);
    }
}

void
ViperRtl::syncStats()
{
    for (int k = 0; k < viper_rtl::ST_NUM; k++) {
        const uint64_t v = rtl->stats[k];
        if (v != lastStats[k]) {
            *rtlStats.byIndex[k] += (v - lastStats[k]);
            lastStats[k] = v;
        }
    }
    rtlStats.rtlCycles = rtl->rtlCycles;
    rtlStats.rtlEvents = rtl->events;
    rtlStats.rtlMaxCyclesPerEvent = rtl->maxCyclesPerEvent;
}

// ---------------------------------------------------------------------
// Env: the RTL's service ports
// ---------------------------------------------------------------------
bool
ViperRtl::snoop(Addr pa, bool secure)
{
    if (hostCache == nullptr) {
        return false;
    }
    return hostCache->inCache(pa, secure) || hostCache->inMissQueue(pa, secure);
}

void
ViperRtl::onMtqAlloc(int id)
{
    // createPrefetchRequest() reads the demand packet's flags and
    // context id; keep them beside the slot.
    SlotMeta &m = slots[id];
    m.gen++;
    m.valid = (curReq != nullptr);
    if (curReq) {
        m.flags = curReq->getFlags();
        m.contextId = curReq->contextId();
    }
}

bool
ViperRtl::startTranslation(int id, Addr va, bool secure, Addr pc,
                           bool pcValid, bool &ok, Addr &pa)
{
    SlotMeta &m = slots[id];
    if (!m.valid || mmu == nullptr) {
        ok = false;
        return true;   // fails synchronously
    }
    RequestPtr req = std::make_shared<Request>(
        va, blkSize, m.flags, requestorId, pcValid ? pc : 0, m.contextId);
    req->setFlags(Request::PREFETCH);
    if (secure) {
        req->setFlags(Request::SECURE);
    }
    auto x = std::make_unique<XlReq>(this, id, m.gen, req);
    XlReq *xp = x.get();
    inflight.push_back(std::move(x));
    ThreadContext *tc = system->threads[m.contextId];
    syncReq = xp;
    mmu->translateTiming(req, tc, xp, BaseMMU::Read);
    syncReq = nullptr;
    if (xp->done) {
        ok = xp->ok;
        pa = ok ? req->getPaddr() : 0;
        return true;
    }
    return false;
}

void
ViperRtl::XlReq::finish(const Fault &fault, const RequestPtr &r,
                        ThreadContext *tc, BaseMMU::Mode mode)
{
    done = true;
    ok = (fault == NoFault);
    if (owner->syncReq == this) {
        return;   // startTranslation reads the result
    }
    owner->translationDone(this);
}

void
ViperRtl::translationDone(XlReq *x)
{
    // Queued::translationComplete: an asynchronous MMU completion
    reapTranslations();
    if (slots[x->id].gen == x->gen) {
        curReq = nullptr;
        rtl->translationComplete(x->id, x->ok, x->ok ? x->req->getPaddr() : 0,
                                 curTick() + clockPeriod() * latency);
        syncStats();
    }
    // otherwise the slot was dropped or re-used: stale, ignored
}

void
ViperRtl::reapTranslations()
{
    for (auto it = inflight.begin(); it != inflight.end();) {
        if ((*it)->done && it->get() != syncReq) {
            it = inflight.erase(it);
        } else {
            ++it;
        }
    }
}

// ---------------------------------------------------------------------
// Announcement sideband (ViperFinal::announcedLimit)
// ---------------------------------------------------------------------
Addr
ViperRtl::announcedLimit(Addr pc, Addr line_va) const
{
    if (!limitGate || announceTbl == nullptr) {
        return 0;
    }
    Addr limit = 0;
    for (const auto &e : announceTbl->entries()) {
        if (e.valid && e.lastPc == pc && e.limitAddr > line_va &&
            (limit == 0 || e.limitAddr < limit)) {
            limit = e.limitAddr;
        }
    }
    return limit;
}

// ---------------------------------------------------------------------
// Self-clocked drain
// ---------------------------------------------------------------------
void
ViperRtl::scheduleDrain()
{
    if (drainEvent.scheduled() || !rtl->workPending()) {
        return;
    }
    schedule(drainEvent, clockEdge(Cycles(1)));
}

void
ViperRtl::drainTick()
{
    reapTranslations();
    curReq = drainCtxReq;
    const bool ran = rtl->drainTick(curTick() + clockPeriod() * latency);
    syncStats();
    // ViperFinal::drainTick re-arms only after it ran the emission
    // engine; a no-op firing waits for the next demand or cache pull.
    if (ran) {
        scheduleDrain();
    }
}

// ---------------------------------------------------------------------
// Base entry points
// ---------------------------------------------------------------------
void
ViperRtl::notify(const CacheAccessProbeArg &acc, const PrefetchInfo &pfi)
{
    RTL_READY();
    reapTranslations();
    const PacketPtr pkt = acc.pkt;
    hostCache = &acc.cache;

    viper_rtl::AccessEvent ev;
    ev.va = pfi.getAddr();
    ev.pa = pfi.getPaddr();
    ev.size = pfi.getSize();
    ev.pcValid = pfi.hasPC();
    ev.pc = ev.pcValid ? pfi.getPC() : 0;
    ev.secure = pfi.isSecure();
    ev.isWrite = pfi.isWrite();
    ev.isMiss = pfi.isCacheMiss();
    ev.ctxValid = pkt != nullptr && pkt->req != nullptr &&
                  pkt->req->hasVaddr() && pkt->req->hasContextId();
    // ViperFinal::notify's hitPkt predicate
    ev.hitDataValid = captureOnReadHit && pkt != nullptr &&
                      !pfi.isCacheMiss() && pkt->isRead() &&
                      !pkt->cmd.isHWPrefetch() && pfi.hasData();
    if (ev.hitDataValid) {
        ev.hitSize = pkt->getSize();
        ev.hitPayload = pkt->getConstPtr<uint8_t>();
    }
    if (ev.pcValid) {
        const VectorChainTable::ProducerInfo info = tbl->producerInfo(ev.pc);
        ev.producerFound = info.found;
        ev.producerStrided = info.stride != 0;
        ev.producerElemBytes = info.elemBytes;
        ev.announcedLimit = announcedLimit(ev.pc, ev.va);
    }
    ev.demandPrefetched = pkt != nullptr &&
        acc.cache.hasBeenPrefetched(pkt->getAddr(), pkt->isSecure());
    ev.readyStamp = curTick() + clockPeriod() * latency;

    curReq = (pkt != nullptr) ? pkt->req : nullptr;
    if (ev.ctxValid) {
        drainCtxReq = pkt->req;
    }
    rtl->notify(ev);
    curReq = nullptr;
    syncStats();
    scheduleDrain();
}

void
ViperRtl::notifyFill(const CacheAccessProbeArg &acc)
{
    RTL_READY();
    const PacketPtr pkt = acc.pkt;
    viper_rtl::FillEvent f;
    f.pa = pkt->getAddr();
    f.secure = pkt->isSecure();
    f.hasData = pkt->hasData();
    f.size = pkt->getSize();
    if (f.hasData) {
        const unsigned n = std::min<unsigned>(f.size, 64);
        std::memcpy(f.data, pkt->getConstPtr<uint8_t>(), n);
    }
    curReq = nullptr;
    rtl->notifyFill(f);
    syncStats();
    scheduleDrain();
}

void
ViperRtl::notifyEvict(const CacheDataUpdateProbeArg &info)
{
    RTL_READY();
    curReq = nullptr;
    rtl->notifyEvict(info.addr);
    syncStats();
}

PacketPtr
ViperRtl::getPacket()
{
    RTL_READY();
    reapTranslations();
    curReq = drainCtxReq;
    const viper_rtl::Popped p = rtl->getPacket(curTick() + clockPeriod() * latency);
    curReq = nullptr;
    syncStats();
    if (!p.valid) {
        scheduleDrain();
        return nullptr;
    }
    // DeferredPacket::createPkt
    RequestPtr req = std::make_shared<Request>(p.pa, blkSize, 0, requestorId);
    if (p.secure) {
        req->setFlags(Request::SECURE);
    }
    req->taskId(context_switch_task_id::Prefetcher);
    PacketPtr pkt = new Packet(req, MemCmd::HardPFReq);
    pkt->allocate();
    if (tagPrefetch && p.pcValid) {
        pkt->req->setPC(p.pc);
    }
    prefetchStats.pfIssued++;
    issuedPrefetches += 1;
    DPRINTF(ViperRtl, "issuing prefetch PA %#x (VA %#x)\n", p.pa, p.va);
    scheduleDrain();
    return pkt;
}

Tick
ViperRtl::nextPrefetchReadyTime() const
{
    if (rtl == nullptr) {
        return MaxTick;
    }
    uint64_t stamp;
    return rtl->nextPrefetchReady(stamp) ? Tick(stamp) : MaxTick;
}

void
ViperRtl::pfHitInCache()
{
    Base::pfHitInCache();
    RTL_READY();
    curReq = nullptr;
    rtl->pfHitInCache();
    syncStats();
}

void
ViperRtl::resetLearnedState()
{
    RTL_READY();
    curReq = nullptr;
    rtl->resetLearnedState();
    syncStats();
}

// ---------------------------------------------------------------------
// FormSink
// ---------------------------------------------------------------------
void
ViperRtl::installForm(Addr producer_pc, int consumer_dct_ptr,
                      unsigned elem_bytes, unsigned nfields,
                      const VectorChainTable::FoldedForm &f)
{
    // Segment (vlseg) producers are not modelled in the RTL port:
    // their forms would read interleaved lines as if unit-stride.
    // Keep them stream-only there, like negated forms.
    if (nfields >= 2) {
        return;
    }
    // The RTL lane has no subtract path: a negated (vrsub) form is
    // not representable there. Such gathers stay stream-only in the
    // RTL co-sim (viper_final proper converts them).
    if (f.negate) {
        return;
    }
    // elem_bytes is not forwarded: the RTL (~/viper_rtl) still keeps
    // the slice width on its STT, taken from the access event
    // (producer_elem_bytes). Moving it onto the RTL's IPT set is a
    // change in that repository.
    RTL_READY();
    viper_rtl::Form rf;
    rf.valid = f.valid;
    rf.base = f.base;
    rf.shift = f.shift;
    rf.rshift = f.rshift;
    rf.rshiftSigned = f.rshiftSigned;
    rf.extCode = f.extCode;
    rf.extSigned = f.extSigned;
    curReq = nullptr;
    rtl->installForm(producer_pc, consumer_dct_ptr, rf);
    syncStats();
}

void
ViperRtl::invalidateForm(Addr producer_pc, int consumer_dct_ptr)
{
    RTL_READY();
    curReq = nullptr;
    rtl->invalidateForm(producer_pc, consumer_dct_ptr);
    syncStats();
}

void
ViperRtl::invalidateAllForms()
{
    RTL_READY();
    curReq = nullptr;
    rtl->invalidateAllForms();
    syncStats();
}

} // namespace prefetch
} // namespace gem5
