/**
 * ViperRtl — the VIPER-FINAL prefetcher as a Verilated RTL model inside
 * gem5 (--prefetcher viper_rtl).
 *
 * The RTL (viper_rtl repo, rtl/) implements ViperFinal AND the Queued
 * prefetch queue, so this class derives from prefetch::Base, not Queued:
 * the queue, the translation bookkeeping and the emission engine live in
 * hardware. This wrapper only
 *   - turns each Base/FormSink entry point into one RTL event through
 *     viper_rtl::Driver (harness/viper_rtl_driver.hh),
 *   - answers the RTL's service ports with gem5 objects: address
 *     translation (the registered MMU), cache snoops (the hosting
 *     cache's CacheAccessor), and the read-hit payload,
 *   - builds the gem5 Packet for a prefetch the RTL pops, and
 *   - mirrors the RTL's statistic pulses onto gem5 stats with the
 *     same names ViperFinal and Queued use, so sweep parsers need no
 *     change.
 *
 * Timing ("exact mode"): all RTL cycles of one event complete inside
 * the gem5 tick that raised it; the RTL cycle count per event is
 * reported (rtlCycles, rtlMaxCyclesPerEvent). See the viper_rtl README.
 */
#ifndef __MEM_CACHE_PREFETCH_VIPER_RTL_HH__
#define __MEM_CACHE_PREFETCH_VIPER_RTL_HH__

#include <list>
#include <memory>
#include <vector>

#include "base/statistics.hh"
#include "cpu/revela_table.hh"
#include "cpu/vector_chain_table.hh"
#include "harness/viper_rtl_driver.hh"
#include "mem/cache/prefetch/base.hh"

namespace gem5
{

struct ViperRtlPrefetcherParams;

namespace prefetch
{

class ViperRtl : public Base, public VectorChainTable::FormSink,
                 private viper_rtl::Env
{
  public:
    ViperRtl(const ViperRtlPrefetcherParams &p);
    ~ViperRtl();

    // ---- Base ----
    void notify(const CacheAccessProbeArg &acc,
                const PrefetchInfo &pfi) override;
    void notifyFill(const CacheAccessProbeArg &acc) override;
    void notifyEvict(const CacheDataUpdateProbeArg &info) override;
    PacketPtr getPacket() override;
    Tick nextPrefetchReadyTime() const override;
    void pfHitInCache() override;
    void resetLearnedState() override;

    // ---- VectorChainTable::FormSink ----
    void installForm(Addr producer_pc, int consumer_dct_ptr,
                     unsigned elem_bytes, unsigned nfields,
                     const VectorChainTable::FoldedForm &f) override;
    void invalidateForm(Addr producer_pc, int consumer_dct_ptr) override;
    void invalidateAllForms() override;

  private:
    // ---- viper_rtl::Env (the RTL's service ports) ----
    bool snoop(Addr pa, bool secure) override;
    bool startTranslation(int id, Addr va, bool secure, Addr pc,
                          bool pcValid, bool &ok, Addr &pa) override;
    void onMtqAlloc(int id) override;

    /** One in-flight MMU translation for translation-queue slot `id`. */
    struct XlReq : public BaseMMU::Translation
    {
        ViperRtl *owner;
        int id;
        uint64_t gen;
        RequestPtr req;
        bool done = false;
        bool ok = false;
        XlReq(ViperRtl *o, int i, uint64_t g, RequestPtr r)
            : owner(o), id(i), gen(g), req(r) {}
        void markDelayed() override {}
        void finish(const Fault &fault, const RequestPtr &r,
                    ThreadContext *tc, BaseMMU::Mode mode) override;
    };
    /** Per translation-queue slot: the context the candidate was
     *  inserted under (what createPrefetchRequest reads off the
     *  demand packet) and a generation to drop stale completions. */
    struct SlotMeta
    {
        uint64_t gen = 0;
        Request::Flags flags;
        ContextID contextId = 0;
        bool valid = false;
    };
    std::vector<SlotMeta> slots;
    std::list<std::unique_ptr<XlReq>> inflight;
    XlReq *syncReq = nullptr;   // the translation being started right now
    void reapTranslations();
    void translationDone(XlReq *x);

    /** Read the announced extent end for the walk (ViperFinal::
     *  announcedLimit) */
    Addr announcedLimit(Addr pc, Addr line_va) const;

    /** Arm the self-clocked drain if the RTL holds conversion work */
    void scheduleDrain();
    void drainTick();
    EventFunctionWrapper drainEvent;

    /** Pull the RTL's stat increments into the gem5 stats */
    void syncStats();

    VectorChainTable *const tbl;
    RevelaStreamTable *const announceTbl;
    const bool limitGate;
    const bool captureOnReadHit;
    const Cycles latency;
    const bool tagPrefetch;
    const std::string traceFileName;
    viper_rtl::Config cfg;
    viper_rtl::Driver *rtl = nullptr;

    /** Request of the event in progress (translation metadata) and the
     *  hosting cache (snoops, residency checks) */
    RequestPtr curReq;
    const CacheAccessor *hostCache = nullptr;
    /** The most recent demand's request: the drain's context */
    RequestPtr drainCtxReq;

    uint64_t lastStats[viper_rtl::ST_NUM] = {};

    struct ViperRtlStats : public statistics::Group
    {
        ViperRtlStats(statistics::Group *parent);
        std::vector<statistics::Scalar *> byIndex;   // viper_rtl::Stat order
        statistics::Scalar rtlCycles;
        statistics::Scalar rtlEvents;
        statistics::Scalar rtlMaxCyclesPerEvent;
    } rtlStats;
};

} // namespace prefetch
} // namespace gem5

#endif // __MEM_CACHE_PREFETCH_VIPER_RTL_HH__
