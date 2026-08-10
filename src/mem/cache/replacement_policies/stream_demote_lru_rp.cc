/**
 * Stream-demoting LRU implementation. See stream_demote_lru_rp.hh for
 * the two demotion modes and cpu/gdp_table.hh for the stream-page
 * registry the classification reads.
 */

#include "mem/cache/replacement_policies/stream_demote_lru_rp.hh"

#include "base/logging.hh"
#include "mem/packet.hh"
#include "params/StreamDemoteLRURP.hh"

namespace gem5
{

namespace replacement_policy
{

StreamDemoteLRU::StreamDemoteLRU(const Params &p)
  : LRU(p),
    tbl(p.link_table),
    demoteOnInsert(p.demote_on_insert),
    secondTouchPromote(p.second_touch_promote),
    pagePromote(p.page_promote),
    sdStats(this)
{
    fatal_if(tbl == nullptr, "%s: no link_table set. StreamDemoteLRU "
             "needs the same GdpChainTable instance the prefetcher and "
             "CPU share (the config script wires all three).", name());
}

StreamDemoteLRU::StreamDemoteStats::StreamDemoteStats(
    statistics::Group *parent)
  : statistics::Group(parent),
    ADD_STAT(demotedInserts, statistics::units::Count::get(),
        "stream lines inserted at the LRU position"),
    ADD_STAT(demotedTouches, statistics::units::Count::get(),
        "stream lines demoted (or held demoted) at a touch"),
    ADD_STAT(promotedTouches, statistics::units::Count::get(),
        "demoted stream lines promoted at a second touch "
        "(observed reuse overrides the single-use oracle)"),
    ADD_STAT(pagesPromoted, statistics::units::Count::get(),
        "pages unlearned at a second touch (removed from the stream "
        "registry and blocked from re-registration)")
{
}

bool
StreamDemoteLRU::isStream(const PacketPtr pkt) const
{
    return pkt != nullptr && tbl->isStreamPage(pkt->getAddr());
}

void
StreamDemoteLRU::demote(const std::shared_ptr<ReplacementData> &data)
{
    // Tick 1 is older than any real touch, so the entry becomes its
    // set's next victim while remaining a valid hit until then.
    std::static_pointer_cast<LRUReplData>(data)->lastTouchTick = 1;
}

void
StreamDemoteLRU::touch(const std::shared_ptr<ReplacementData>
                       &replacement_data, const PacketPtr pkt)
{
    if (isStream(pkt)) {
        auto sd = std::static_pointer_cast<SdReplData>(replacement_data);
        if (secondTouchPromote && sd->consumed) {
            // Second consumption of a line the oracle declared dead,
            // while still resident: observed reuse (iterative
            // re-sweep) overrides the per-sweep single-use premise.
            LRU::touch(replacement_data);
            sdStats.promotedTouches++;
            if (pagePromote) {
                // Unlearn the whole page: per-line promotion protects
                // incumbents only — a re-fetched line re-enters
                // demoted and is re-victimized before its second
                // touch (churn lockout). Blocking the page's
                // re-registration lets new fills insert as plain LRU.
                tbl->promoteStreamPage(pkt->getAddr());
                sdStats.pagesPromoted++;
            }
        } else {
            // A unit-stride vector access consumes the line's spatial
            // locality whole: this touch is the line's known last use
            // (L1 mode), or an incidental re-read that must not
            // resurrect a dead-on-arrival copy (L2 mode).
            sd->consumed = true;
            demote(replacement_data);
            sdStats.demotedTouches++;
        }
    } else {
        LRU::touch(replacement_data);
    }
}

void
StreamDemoteLRU::reset(const std::shared_ptr<ReplacementData>
                       &replacement_data, const PacketPtr pkt)
{
    LRU::reset(replacement_data);
    // Protection is per-residency: a re-fetched line must earn its
    // second touch again.
    std::static_pointer_cast<SdReplData>(replacement_data)->consumed =
        false;
    if (demoteOnInsert && isStream(pkt)) {
        demote(replacement_data);
        sdStats.demotedInserts++;
    }
}

void
StreamDemoteLRU::invalidate(const std::shared_ptr<ReplacementData>
                            &replacement_data)
{
    LRU::invalidate(replacement_data);
    std::static_pointer_cast<SdReplData>(replacement_data)->consumed =
        false;
}

std::shared_ptr<ReplacementData>
StreamDemoteLRU::instantiateEntry()
{
    return std::shared_ptr<ReplacementData>(new SdReplData());
}

} // namespace replacement_policy
} // namespace gem5
