/**
 * @file
 * Declaration of a stream-demoting LRU replacement policy.
 *
 * LRU, except that lines belonging to registered STREAM pages (single
 * -use index/data arrays walked by a vector kernel — pages published
 * by the VTyche/GDP prefetcher through the shared VectorChainTable) are
 * demoted to the LRU position instead of being promoted:
 *
 *  - demote_on_insert = true  (L2 use): a stream line inserts already
 *    at LRU. Safe there because the fill has served its purpose on the
 *    way past — the L1 copy handles the demand use — so the L2 copy is
 *    dead on arrival and should yield capacity to reused data (e.g.,
 *    the gather-target vector in SpMV).
 *  - demote_on_insert = false (L1 use): a stream line inserts normally
 *    (it still owes its one demand use) and is demoted when touched —
 *    a unit-stride vector access consumes a line's spatial locality in
 *    that single access, making the touch an architecturally known
 *    last use.
 *
 * Non-stream lines behave exactly as LRU. With no table wired or a
 * packet-less call, the policy degrades to plain LRU.
 *
 * SECOND-TOUCH PROMOTION (second_touch_promote): the "single-use"
 * premise is per-sweep truth — iterative kernels re-sweep the same
 * arrays (timesteps, repeat-runs, phases), and unconditional demotion
 * then evicts lines that observably come back (blackscholes: 41x DRAM
 * reads). Each line carries one CONSUMED bit, valid for its current
 * residency only: the first touch demotes and sets it (the
 * architecturally known consumption, unchanged); a touch on a line
 * whose bit is already set is a SECOND consumption of a line the
 * oracle declared dead — recorded fact, not prediction — and promotes
 * it as plain LRU (promotedTouches). True single-use streams never see
 * a second touch, so every demotion win is preserved; protection must
 * be re-earned per residency (eviction clears the bit), so the
 * mechanism cannot over-protect.
 */

#ifndef __MEM_CACHE_REPLACEMENT_POLICIES_STREAM_DEMOTE_LRU_RP_HH__
#define __MEM_CACHE_REPLACEMENT_POLICIES_STREAM_DEMOTE_LRU_RP_HH__

#include "cpu/vector_chain_table.hh"
#include "mem/cache/replacement_policies/lru_rp.hh"

namespace gem5
{

struct StreamDemoteLRURPParams;

namespace replacement_policy
{

class StreamDemoteLRU : public LRU
{
  protected:
    /** LRU data plus the per-residency consumed bit */
    struct SdReplData : LRUReplData
    {
        /** This copy has received its architecturally-final touch */
        bool consumed = false;
    };

  private:
    /** The per-core chain table publishing stream physical pages */
    VectorChainTable *const tbl;
    /** Demote at insertion (L2 semantics) vs at first touch (L1) */
    const bool demoteOnInsert;
    /** Promote a demoted line on its second touch (observed reuse
     *  overrides the single-use oracle) */
    const bool secondTouchPromote;
    /** Second touch also unlearns the whole PAGE (VectorChainTable
     *  promoteStreamPage): new fills of a proven-reused page insert
     *  as plain LRU — the churn set's re-entry path that per-line
     *  promotion alone cannot provide */
    const bool pagePromote;

    bool isStream(const PacketPtr pkt) const;
    /** Age the entry to the LRU position of its set */
    static void demote(const std::shared_ptr<ReplacementData> &data);

    struct StreamDemoteStats : public statistics::Group
    {
        StreamDemoteStats(statistics::Group *parent);
        /** Stream lines inserted at the LRU position */
        statistics::Scalar demotedInserts;
        /** Stream lines demoted (or held demoted) at a touch */
        statistics::Scalar demotedTouches;
        /** Demoted lines promoted at a second touch (observed reuse) */
        statistics::Scalar promotedTouches;
        /** Pages unlearned (promoted out of the stream class) */
        statistics::Scalar pagesPromoted;
    } sdStats;

  public:
    typedef StreamDemoteLRURPParams Params;
    StreamDemoteLRU(const Params &p);
    ~StreamDemoteLRU() = default;

    using LRU::touch;
    void touch(const std::shared_ptr<ReplacementData> &replacement_data,
               const PacketPtr pkt) override;

    using LRU::reset;
    void reset(const std::shared_ptr<ReplacementData> &replacement_data,
               const PacketPtr pkt) override;

    /** Clear the consumed bit with the residency it belongs to */
    void invalidate(const std::shared_ptr<ReplacementData>
                    &replacement_data) override;

    std::shared_ptr<ReplacementData> instantiateEntry() override;
};

} // namespace replacement_policy
} // namespace gem5

#endif // __MEM_CACHE_REPLACEMENT_POLICIES_STREAM_DEMOTE_LRU_RP_HH__
