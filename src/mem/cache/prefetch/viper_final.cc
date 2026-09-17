/**
 * ViperFinal (VIPER-FINAL) implementation: the shipping subset of
 * Viper. See viper_final.hh for what is removed relative to
 * mem/cache/prefetch/viper.{hh,cc}, and cpu/vector_chain_table.hh for
 * the CPU-side discovery half it shares with GDP and Viper.
 */

#include "mem/cache/prefetch/viper_final.hh"

#include <algorithm>
#include <cassert>
#include <cstring>
#include <iterator>

#include "base/intmath.hh"
#include "base/logging.hh"
#include "base/trace.hh"
#include "debug/ViperFinal.hh"
#include "params/ViperFinalPrefetcher.hh"
#include "sim/byteswap.hh"

namespace gem5
{

namespace prefetch
{

ViperFinal::ViperFinal(const ViperFinalPrefetcherParams &p)
  : Queued(p),
    tbl(p.link_table),
    announceTbl(p.stream_table),
    limitGate(p.limit_gate),
    prefetchDistance(p.prefetch_distance),
    streamOnly(p.stream_only),
    indexLineQueueEntries(p.index_line_queue_entries),
    producerSlots(p.producer_slots),
    irtEntries(p.routing_entries),
    captureOnReadHit(p.capture_on_read_hit),
    retiredTailEntries(p.retired_tail_entries),
    captureReadHitOwnProducer(p.capture_read_hit_own_producer),
    captureQueueGate(p.capture_queue_gate),
    dropBatchConfidence(p.drop_batch_confidence),
    dropBatchFraction(p.drop_batch_fraction),
    drainFloor(p.drain_floor),
    dedupBufferSize(p.dedup_buffer_size),
    evictDedupEntries(p.evict_dedup_entries),
    conversionLanes(p.conversion_lanes),
    dropOnFull(p.drop_on_full),
    stridedProducers(p.strided_producers),
    segmentProducers(p.segment_producers),
    preLaneDedup(p.pre_lane_dedup),
    seenSetEntries(p.seen_set_entries),
    seenSetDedup(p.seen_set_dedup),
    consumersPerProducer(p.consumers_per_producer),
    randomDropInterval(p.random_drop_interval),
    randomDropRates(p.random_drop_rates),
    randomDropGeometric(p.random_drop_geometric),
    randomDropMinInterval(p.random_drop_min_interval),
    randomDropRng(p.random_drop_seed),
    streamTrackingTable(),
    indexRoutingTable(),
    viperStats(this),
    drainEvent([this] { drainTick(); }, name())
{
    fatal_if(tbl == nullptr, "%s: no link_table set. ViperFinal needs the "
             "VectorChainTable that is also attached to the CPU's "
             "vector_chain_table param (the config script wires both).", name());
    fatal_if(!tbl->foldedFormsEnabled(),
             "%s: the VectorChainTable must have folded_forms on. "
             "VIPER-FINAL adopts the forms the table's backward walk "
             "pushes into its IPT; it has no op-list path of its own "
             "(the config script sets it for --prefetcher viper_final).",
             name());
    fatal_if(limitGate && announceTbl == nullptr,
             "%s: limit_gate needs the RevelaStreamTable announcement "
             "sideband (stream_table; the config script wires it when "
             "the gate is enabled).", name());
    fatal_if(prefetchDistance < 1,
             "prefetch_distance must be >= 1 cache line (VIPER-FINAL has "
             "no streaming_distance fallback walk)");
    fatal_if(indexLineQueueEntries < 1, "index_line_queue_entries must be >= 1");
    fatal_if(producerSlots < 1, "producer_slots must be >= 1");
    fatal_if(drainFloor < 0, "drain_floor must be >= 0");
    fatal_if(consumersPerProducer < 1 || consumersPerProducer > 255,
             "consumers_per_producer must be in [1, 255]");
    fatal_if(dropBatchConfidence != 0 &&
             (dropBatchFraction <= 0.0 || dropBatchFraction > 1.0),
             "drop_batch_fraction must be in (0, 1]");
    fatal_if(randomDropInterval != 0 && randomDropRates.empty(),
             "random_drop_rates must be non-empty when "
             "random_drop_interval is set");
    for (double r : randomDropRates) {
        fatal_if(r < 0.0 || r > 1.0,
                 "random_drop_rates entries must be in [0, 1]");
    }

    // The IPT lives here, so the discovery half is told where to
    // push folded forms and invalidates. One set per producer slot:
    // holding a set IS holding a slot.
    ipt.resize(producerSlots);
    tbl->attachFormSink(this);
}

void
ViperFinal::resetLearnedState()
{
    // The VectorChainTable registers its own reset callback, and its
    // clear pushes invalidateAllForms here, which empties the IPT.
    // Only this prefetcher's runtime state is cleared here.
    streamTrackingTable.clear();
    indexRoutingTable.clear();
    evictDedupLru.clear();
    evictDedupByVa.clear();
    evictDedupByPa.clear();
    // Epoch state restarts drop-free; the RNG stream itself continues
    // (its draw sequence is not learned state).
    notifiesSinceDraw = 0;
    currentDropScale = 0.0;
}

ViperFinal::ViperFinalStats::ViperFinalStats(statistics::Group *parent)
  : statistics::Group(parent),
    ADD_STAT(chunksObserved, statistics::units::Count::get(),
        "chunk events at producer PCs"),
    ADD_STAT(streamCandidates, statistics::units::Count::get(),
        "index-array stream candidates emitted"),
    ADD_STAT(streamLimitClamped, statistics::units::Count::get(),
        "walk lines suppressed at the announced extent end "
        "(limit_gate)"),
    ADD_STAT(streamLimitNoAnnounce, statistics::units::Count::get(),
        "accesses whose whole walk was suppressed: no live announcement "
        "covered them (limit_gate)"),
    ADD_STAT(formsInstalled, statistics::units::Count::get(),
        "folded forms pushed in by the discovery half (a way became "
        "live, or a live way was refreshed)"),
    ADD_STAT(iptSetConflicts, statistics::units::Count::get(),
        "pushes dropped: no IPT set free for that producer "
        "(the producer_slots cap)"),
    ADD_STAT(iptWaysExhausted, statistics::units::Count::get(),
        "pushes dropped: the producer's IPT ways are all taken"),
    ADD_STAT(formsInvalidated, statistics::units::Count::get(),
        "consumer ways dropped by a per-gather invalidate pulse "
        "(an operand the form was folded from changed)"),
    ADD_STAT(globalInvalidates, statistics::units::Count::get(),
        "broadcast invalidates taken: the chain table was cleared, or "
        "a changed operand's head could not be resolved"),
    ADD_STAT(capturesRegistered, statistics::units::Count::get(),
        "index lines registered for capture (a departing stream "
        "prefetch inside a live producer's window)"),
    ADD_STAT(fillsCaptured, statistics::units::Count::get(),
        "captured index-line fills latched into the index line queue"),
    ADD_STAT(hitsCaptured, statistics::units::Count::get(),
        "resident index lines latched off the read port on a demand "
        "read hit (capture_on_read_hit)"),
    ADD_STAT(stridedChunks, statistics::units::Count::get(),
        "chunk events at strided (vlse) producers (span model)"),
    ADD_STAT(stridedHitsCaptured, statistics::units::Count::get(),
        "resident lines read off the cache and latched on a strided "
        "producer's element read hit"),
    ADD_STAT(stridedUnsupported, statistics::units::Count::get(),
        "accesses at strided producers refused: stride below EEW, not "
        "a multiple of it, or EEW not dividing the line"),
    ADD_STAT(stridedWalkGated, statistics::units::Count::get(),
        "strided chunk events whose stream walk was skipped because "
        "the producer has no linked gather (chain-only trigger)"),
    ADD_STAT(segmentLinesCaptured, statistics::units::Count::get(),
        "index lines captured at segment (vlseg) producers, carrying "
        "a field base mask"),
    ADD_STAT(segmentPhaseUnknown, statistics::units::Count::get(),
        "segment lines captured before the macro base was known "
        "(phase assumed 0)"),
    ADD_STAT(segmentUnsupported, statistics::units::Count::get(),
        "accesses at segment producers refused (segment_producers = "
        "false, or nf/EEW outside the 64-slot line model)"),
    ADD_STAT(fieldSlotsSkipped, statistics::units::Count::get(),
        "(way, slot) pairs skipped because the slot holds another "
        "field's element (no lane fired, no budget charged)"),
    ADD_STAT(walkRestarts, statistics::units::Count::get(),
        "unit-stride chunk events behind the cursor that reset the "
        "stream walk"),
    ADD_STAT(segmentReorderTolerated, statistics::units::Count::get(),
        "segment-producer chunk events behind the cursor but inside "
        "the macro's span, treated as fragment reordering (no reset)"),
    ADD_STAT(recapturesDetected, statistics::units::Count::get(),
        "read-hit captures of a line this same producer had already "
        "converted (retired IRT tail hit)"),
    ADD_STAT(recapturesSuppressed, statistics::units::Count::get(),
        "of those, the captures actually skipped "
        "(capture_read_hit_own_producer = false)"),
    ADD_STAT(capturesGated, statistics::units::Count::get(),
        "capture admissions skipped at a congested prefetch queue "
        "(capture_queue_gate)"),
    ADD_STAT(batchDropSamples, statistics::units::Count::get(),
        "resident-drops attributed to a batch (drop_batch_confidence)"),
    ADD_STAT(batchesAborted, statistics::units::Count::get(),
        "batches aborted at the drop_batch_confidence threshold"),
    ADD_STAT(targetsFlushedOnDrop, statistics::units::Count::get(),
        "targets flushed from the queue or suppressed at emission "
        "because their batch aborted"),
    ADD_STAT(bufferBusyDrops, statistics::units::Count::get(),
        "fills dropped with the index line queue full"),
    ADD_STAT(noFormSkips, statistics::units::Count::get(),
        "fills refused and latched targets forfeited: the producer, or "
        "the one way that made them, holds no form"),
    ADD_STAT(elementsConverted, statistics::units::Count::get(),
        "elements pushed through a shift-and-add lane"),
    ADD_STAT(conversionWidthLimited, statistics::units::Count::get(),
        "conversion events cut short by the lane budget (the line "
        "resumed on a later event)"),
    ADD_STAT(epochRedraws, statistics::units::Count::get(),
        "epoch scale redraws taken by the random tail-drop clock"),
    ADD_STAT(linesRandomTruncated, statistics::units::Count::get(),
        "captured lines the random tail-drop throttle truncated"),
    ADD_STAT(elementsRandomDropped, statistics::units::Count::get(),
        "elements forfeited off truncated lines' tails (never "
        "reached a shift/add lane)"),
    ADD_STAT(slotsCompacted, statistics::units::Count::get(),
        "elements compacted out of the lane budget: every live way's "
        "seen-set hit before the lanes (pre_lane_dedup), so no slot "
        "was spent"),
    ADD_STAT(targetsGenerated, statistics::units::Count::get(),
        "indirect target candidates generated"),
    ADD_STAT(targetsFiltered, statistics::units::Count::get(),
        "targets dropped by the canonical-address filter"),
    ADD_STAT(targetsDeduplicated, statistics::units::Count::get(),
        "elements dropped as duplicate target lines by the per-way "
        "seen-set (one source line, or the persistent CAM at "
        "seen_set_entries > 0)"),
    ADD_STAT(seenSetReplaced, statistics::units::Count::get(),
        "persistent seen-set keys replaced FIFO because the CAM was "
        "full (seen_set_entries > 0 only)"),
    ADD_STAT(targetsCrossDeduplicated, statistics::units::Count::get(),
        "targets dropped: line emitted within the dedup window"),
    ADD_STAT(targetsEvictDeduplicated, statistics::units::Count::get(),
        "targets suppressed: line tracked by the eviction-clocked "
        "dedup (emitted earlier, not seen leave the cache since)"),
    ADD_STAT(evictDedupEvictCleared, statistics::units::Count::get(),
        "tracked lines cleared by a cache eviction (re-armed)"),
    ADD_STAT(evictDedupMissCleared, statistics::units::Count::get(),
        "tracked lines cleared by an observed demand miss"),
    ADD_STAT(evictDedupReplaced, statistics::units::Count::get(),
        "tracked lines replaced at capacity (LRU-by-access)"),
    ADD_STAT(targetsDroppedFull, statistics::units::Count::get(),
        "targets dropped at a full queue (drop_on_full) instead of "
        "displacing queued entries"),
    ADD_STAT(emissionDeferred, statistics::units::Count::get(),
        "emission events paused by a full queue (the stall firing)"),
    ADD_STAT(emissionLineLimited, statistics::units::Count::get(),
        "events where the array's one-line conversion slot ended "
        "with further lines still buffered"),
    ADD_STAT(selfDrainTicks, statistics::units::Count::get(),
        "self-clocked drain firings that ran the emission engine")
{
}

// ---------------------------------------------------------------------
// The lane: target = base + ((extend(elem) >> rshift) << shift)
// ---------------------------------------------------------------------

uint64_t
ViperFinal::extendRaw(const Form &f, uint64_t raw) const
{
    // extCode is the 3-bit way field; decode it to the width the
    // extension starts from (0 = none, 64 = already full width).
    const unsigned bits = f.extWidth();
    if (bits > 0 && bits < 64) {
        if (f.extSigned) {
            const unsigned sh = 64 - bits;
            raw = (uint64_t)((int64_t)(raw << sh) >> sh);
        } else {
            raw &= (1ULL << bits) - 1;
        }
    }
    return raw;
}

Addr
ViperFinal::applyForm(const Form &f, uint64_t raw) const
{
    // The lane: one funnel shifter (right, then left), one adder.
    uint64_t v = extendRaw(f, raw);
    if (f.rshift) {
        v = f.rshiftSigned ? (uint64_t)((int64_t)v >> f.rshift)
                           : (v >> f.rshift);
    }
    // negate: the adder subtracts the index term (vrsub chains).
    const uint64_t term = v << f.shift;
    return f.base + (f.negate ? (0 - term) : term);
}

Addr
ViperFinal::preLaneKey(const Form &f, uint64_t raw) const
{
    // Same target line <=> equal key: line = key + (base >> 6)
    // exactly, because the base's sub-line offset is folded in (in
    // hardware once at CAM insertion: a 6-bit constant add, never a
    // full-width base add). This is the seen-set's only key, in
    // either dedup placement.
    uint64_t v = extendRaw(f, raw);
    if (f.rshift) {
        v = f.rshiftSigned ? (uint64_t)((int64_t)v >> f.rshift)
                           : (v >> f.rshift);
    }
    const uint64_t term = v << f.shift;
    return ((f.negate ? (0 - term) : term) + (f.base & (blkSize - 1)))
           >> floorLog2(blkSize);
}

bool
ViperFinal::inDedupWindow(const ConsumerGroup &cg, Addr line) const
{
    for (const auto &prev : cg.dedupWindow) {
        if (std::find(prev.begin(), prev.end(), line) != prev.end()) {
            return true;
        }
    }
    return false;
}

void
ViperFinal::clearLineState(SttEntry &ps)
{
    // The seen-set is per-line bookkeeping only at the per-line
    // scope; a persistent CAM (seen_set_entries > 0) keeps its keys.
    for (auto &cg : ps.groups) {
        if (seenSetEntries == 0) {
            cg.lineSeen.clear();
        }
        cg.pendingEmitted.clear();
    }
}

void
ViperFinal::clearSeenSets(SttEntry &ps)
{
    for (auto &cg : ps.groups) {
        cg.lineSeen.clear();
    }
}

void
ViperFinal::clearWaySeenSet(Addr producer_pc, unsigned way)
{
    if (seenSetEntries == 0) {
        return; // per-line scope: the next line clears it anyway
    }
    auto it = streamTrackingTable.find(producer_pc);
    if (it != streamTrackingTable.end() &&
        way < it->second.groups.size()) {
        it->second.groups[way].lineSeen.clear();
    }
}

// Two forms fold an index to the same seen-set key iff every operand
// preLaneKey reads is equal; a way's keys survive a re-push only then.
static bool
sameKeying(const VectorChainTable::FoldedForm &a,
           const VectorChainTable::FoldedForm &b)
{
    return a.valid && b.valid && a.base == b.base && a.shift == b.shift
        && a.rshift == b.rshift && a.rshiftSigned == b.rshiftSigned
        && a.extCode == b.extCode && a.extSigned == b.extSigned;
}

void
ViperFinal::closeLineDedup(SttEntry &ps)
{
    // A dedup-window entry spans the SOURCE LINE, not one lane-budget
    // chunk. The contribution enters the window even when empty —
    // the window ages by processed lines.
    for (auto &cg : ps.groups) {
        if (dedupBufferSize) {
            cg.dedupWindow.push_back(std::move(cg.pendingEmitted));
            while (cg.dedupWindow.size() > dedupBufferSize) {
                cg.dedupWindow.pop_front();
            }
        }
        cg.pendingEmitted.clear();
    }
}

void
ViperFinal::flushConversion(SttEntry &ps)
{
    ps.indexLineQueue.clear();
    ps.latch.clear();
    clearLineState(ps);
    // The forms the seen-set keys were folded under are gone with
    // the producer's last way, so a persistent CAM empties too.
    clearSeenSets(ps);
}

// ---------------------------------------------------------------------
// The IPT: forms pushed by the discovery half, read by the lane array
// ---------------------------------------------------------------------

bool
ViperFinal::iptOccupied(const IptSet &st)
{
    for (const IptWay &w : st.ways) {
        if (w.form.valid) {
            return true;
        }
    }
    return false;
}

int
ViperFinal::iptFind(Addr pc) const
{
    for (int i = 0; i < (int)ipt.size(); i++) {
        // A free set keeps a stale tag, so occupancy is tested too.
        if (ipt[i].pcTag == pc && iptOccupied(ipt[i])) {
            return i;
        }
    }
    return -1;
}

int
ViperFinal::iptAlloc(Addr pc)
{
    const int hit = iptFind(pc);
    if (hit >= 0) {
        return hit;
    }
    for (int i = 0; i < (int)ipt.size(); i++) {
        if (!iptOccupied(ipt[i])) {
            ipt[i] = IptSet();
            ipt[i].pcTag = pc;
            return i;
        }
    }
    viperStats.iptSetConflicts++;
    return -1;
}

const ViperFinal::Form *
ViperFinal::wayForm(Addr pc, unsigned way) const
{
    const int si = iptFind(pc);
    if (si < 0 || way >= ipt[si].ways.size() ||
        !ipt[si].ways[way].form.valid) {
        return nullptr;
    }
    return &ipt[si].ways[way].form;
}

void
ViperFinal::installForm(Addr producer_pc, int consumer_dct_ptr,
                        unsigned elem_bytes, unsigned nfields,
                        const Form &f)
{
    if (streamOnly) {
        return; // ablation: no producer is ever live
    }
    // The walk completes a form (base armed, valid set last) before
    // it pushes; a way's liveness IS this bit.
    assert(f.valid);
    const int si = iptAlloc(producer_pc);
    if (si < 0) {
        return; // no set free: this producer streams
    }
    IptSet &st = ipt[si];
    // The slice width is the producer's, not the way's: every push
    // for this set carries the same value (the head row's), so a
    // fresh set takes it here and a re-push rewrites it unchanged.
    st.elemBytes = (uint8_t)elem_bytes;
    // Likewise the field count: a property of the producer, the same
    // on every push for this set (0 on a plain unit-stride head).
    st.nfields = (uint8_t)nfields;
    // The same gather again (a re-walk after its way was cleared):
    // refresh its own way, so the way index — and the dedup record
    // the engine keeps beside it — stays put. A cleared way's pointer
    // is stale but still names the gather that last held it.
    // A persistent seen-set holds keys folded under the way's
    // previous form; they name target lines only under the same
    // operands, so any push that changes them empties the way's CAM.
    for (unsigned wi = 0; wi < st.ways.size(); wi++) {
        IptWay &w = st.ways[wi];
        if (w.consumerDctPtr == consumer_dct_ptr) {
            if (!sameKeying(w.form, f)) {
                clearWaySeenSet(producer_pc, wi);
            }
            w.form = f;
            viperStats.formsInstalled++;
            return;
        }
    }
    for (unsigned wi = 0; wi < st.ways.size(); wi++) {
        IptWay &w = st.ways[wi];
        if (!w.form.valid) {
            clearWaySeenSet(producer_pc, wi); // a different gather
            w.consumerDctPtr = consumer_dct_ptr;
            w.form = f;
            viperStats.formsInstalled++;
            return;
        }
    }
    if (st.ways.size() < consumersPerProducer) {
        clearWaySeenSet(producer_pc, st.ways.size());
        st.ways.push_back(IptWay{consumer_dct_ptr, f});
        viperStats.formsInstalled++;
    } else {
        viperStats.iptWaysExhausted++;
    }
}

void
ViperFinal::invalidateForm(Addr producer_pc, int consumer_dct_ptr)
{
    const int si = iptFind(producer_pc);
    if (si < 0) {
        return;
    }
    IptSet &st = ipt[si];
    for (IptWay &w : st.ways) {
        if (w.consumerDctPtr != consumer_dct_ptr || !w.form.valid) {
            continue;
        }
        w.form = Form();
        viperStats.formsInvalidated++;
        if (!iptOccupied(st)) {
            // The last way is gone, so the set — the producer slot —
            // is free, and the work buffered behind it has no form to
            // convert with. Flush it here, at the one event that
            // makes it dead, rather than test for it on every drain.
            auto it = streamTrackingTable.find(producer_pc);
            if (it != streamTrackingTable.end()) {
                flushConversion(it->second);
            }
        }
        return;
    }
}

void
ViperFinal::invalidateAllForms()
{
    // The table cleared (or an operand moved on a chain whose head
    // could not be named), so every form was folded from operands
    // that no longer exist. One pulse, one flash-clear: the sets and
    // the buffered work behind them.
    std::fill(ipt.begin(), ipt.end(), IptSet());
    for (auto &kv : streamTrackingTable) {
        flushConversion(kv.second);
    }
    viperStats.globalInvalidates++;
}

// ---------------------------------------------------------------------
// Stream walk and capture registration (shared with GDP by design)
// ---------------------------------------------------------------------

Addr
ViperFinal::announcedLimit(Addr pc, Addr line_va) const
{
    // Same aliasing rule as vhybrid.cc: one PC can have several live
    // entries; take the SMALLEST limit still above the line — the
    // most conservative clamp; a mis-attribution can only suppress
    // tail work (a lost prefetch), never fabricate extra.
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

void
ViperFinal::streamAhead(SttEntry &ps, Addr addr, unsigned size,
                         Addr announced_limit,
                         std::vector<AddrPriority> &addresses)
{
    // Never gated on queue room: stream candidates are regenerable and
    // are the engine's fuel — every capture comes from one. Gating them
    // is a positive-feedback collapse, because the queue drains via
    // cache pulls, which starve exactly when misses rise.
    // Line-pinned frontier: the window sits prefetchDistance cache
    // lines ahead of the demand chunk's start and is as wide as the
    // demand advance, so partial-vl chunks (short rows, loop tails)
    // keep the full byte lookahead and consecutive windows tile
    // without gaps whatever the access size (LMUL, VLEN). A
    // fresh/reset window starts at the frontier by construction. This
    // is the only window policy: the access-size-relative
    // streaming_distance walk is not part of VIPER-FINAL, and the
    // knob is not VLEN-relative either (whole-VLEN chunks until
    // 2026-09-12), so the lookahead means the same number of bytes at
    // every VLEN and every vl.
    // Segment (vlseg) producers consume nf times the index bytes per
    // element of one field, so the frontier scales by nf to keep the
    // same lookahead in ELEMENTS -- the rule the strided walk already
    // applies (streamAheadStrided: bytes of index data scaled by the
    // stride). Measured without it on seg1: the 8-line window sat
    // inside the current strip's second memory micro, whose lines the
    // LSQ was already issuing, so half the index lines never
    // registered (coverage 0.50). Unit-stride (nf <= 1): scale 1.
    const Addr scale = ps.nfields >= 2 ? ps.nfields : 1;
    const Addr from = addr + (Addr)prefetchDistance * (Addr)blkSize * scale;
    const Addr to = from + size;
    Addr line = blockAddress(from);
    if (limitGate && announced_limit == 0) {
        // No live announcement covers this access, so the gate has no
        // sanctioned extent for the walk to run into. Emit nothing
        // rather than speculating past the last announced end: the
        // frontier sits prefetch_distance lines ahead of the demand
        // cursor, i.e. ahead of what the core has even issued, so
        // "no announcement" is the NORMAL state out there (measured on
        // CSR spmv: the gate bound on only ~5% of accesses and was
        // inert on the rest, which is exactly where the walk runs
        // furthest). Letting those lines through made the gate a
        // partial clamp that cost timeliness without bounding the
        // walk. Note this bounds only the WALK: a captured line is
        // always converted whole, because it was captured from
        // inside a producer's own window (or a demand miss at a
        // producer PC) and its bytes are index data by construction.
        viperStats.streamLimitClamped +=
            (to - line + blkSize - 1) / blkSize;
        viperStats.streamLimitNoAnnounce++;
        return;
    }
    // Segment producers: the nf memory micros' line fragments
    // interleave at the cache (traced on seg1: A, A+512, A+64, A+576,
    // ...), so "below the high-water mark" no longer means "already
    // walked": micro 1's first fragment raised the mark past micro 0's
    // remaining targets and half the index lines were never pushed.
    // Each fragment maps to exactly one distinct line by its offset in
    // the macro, so for nf >= 2 every fragment pushes its line
    // unfiltered -- the same one-line-per-access shape as a vle. A
    // replayed fragment re-pushes a line the queue filter or the
    // resident-line check then drops. The mark is still raised: it
    // bounds the capture window at issue (getPacket).
    const bool filter = ps.nfields < 2;
    for (; line < to; line += blkSize) {
        if (announced_limit != 0 && line >= announced_limit) {
            // The walk reached the announced extent end: everything
            // past it is adjacent non-index data. Don't raise the
            // high-water mark past it either — a later extent at the
            // same PC restarts the walk from its own accesses.
            viperStats.streamLimitClamped +=
                (to - line + blkSize - 1) / blkSize;
            break;
        }
        if (!filter || line >= ps.limitAddr) {
            addresses.push_back(AddrPriority(line, 0));
            viperStats.streamCandidates++;
        }
    }
    if (line > ps.limitAddr) {
        ps.limitAddr = line;
    }
}

void
ViperFinal::registerCapture(Addr line_pa, Addr line_va, Addr producer_pc,
                             bool secure, uint64_t slot_mask)
{
    for (const IrtEntry &c : indexRoutingTable) {
        if (c.linePaddr == line_pa && c.secure == secure) {
            return; // already watched
        }
    }
    if (indexRoutingTable.size() >= irtEntries) {
        indexRoutingTable.pop_front();
    }
    indexRoutingTable.push_back({line_pa, line_va, producer_pc, secure,
                                 slot_mask});
    viperStats.capturesRegistered++;
}

// ---------------------------------------------------------------------
// Strided (vlse) producers (2026-09-12)
// ---------------------------------------------------------------------

uint64_t
ViperFinal::strideSlotMask(Addr line, unsigned width, int64_t stride,
                           Addr chunk_start) const
{
    // Offset of the line's first element: the element grid is
    // chunk_start + j*stride, so inside this line it starts at
    // (chunk_start - line) mod stride (smallest non-negative residue).
    const int64_t d = (int64_t)chunk_start - (int64_t)line;
    int64_t phase = d % stride;
    if (phase < 0) {
        phase += stride;
    }
    uint64_t mask = 0;
    for (int64_t off = phase; off < (int64_t)blkSize; off += stride) {
        mask |= 1ULL << (off / width);
    }
    return mask;
}

uint64_t
ViperFinal::segmentSlotMask(Addr line, unsigned width, unsigned nfields,
                            Addr base) const
{
    // Field 0's elements sit at base + j*stride, stride = nf*width.
    // Inside this line the grid starts at phase = (base - line) mod
    // stride (smallest non-negative residue) -- the low bits of base
    // when the stride divides the line, a small constant-modulo
    // otherwise (nf = 3, 5, 6, 7). In slots: one bit every nf slots,
    // starting at phase/width. Closed form so the hardware is a
    // pattern select and one barrel shift per line.
    const int64_t stride = (int64_t)nfields * (int64_t)width;
    int64_t phase = ((int64_t)base - (int64_t)line) % stride;
    if (phase < 0) {
        phase += stride;
    }
    const unsigned phase_slot = (unsigned)(phase / width);
    const unsigned slots = blkSize / width;
    uint64_t pattern = 0;
    for (unsigned s = 0; s < slots; s += nfields) {
        pattern |= 1ULL << s;
    }
    return pattern << phase_slot;
}

uint64_t
ViperFinal::fieldSlotMask(uint64_t base_mask, unsigned nfields,
                          unsigned field)
{
    // Rotate the field-0 mask by `field` inside its nf-slot period:
    // field f's elements are f slots after field 0's, and the slot
    // (if any) that wrapped below the first field-0 slot belongs to
    // the previous element's field f, i.e. nf - f slots BEFORE the
    // first field-0 slot. Bits shifted past the line are beyond the
    // element loop and harmless.
    if (nfields < 2 || field == 0 || field >= nfields) {
        return base_mask;
    }
    return (base_mask << field) | (base_mask >> (nfields - field));
}

uint64_t
ViperFinal::captureSlotMask(SttEntry &ps, Addr line_va)
{
    if (ps.stride != 0) {
        return strideSlotMask(line_va, ps.strideElemBytes, ps.stride,
                              ps.chunkStart);
    }
    if (ps.nfields >= 2 && ps.segElemBytes != 0) {
        viperStats.segmentLinesCaptured++;
        if (!ps.segBaseValid) {
            viperStats.segmentPhaseUnknown++;
        }
        // Without a captured base the grid is assumed line-aligned
        // (phase 0): right whenever the array is stride-aligned.
        return segmentSlotMask(line_va, ps.segElemBytes, ps.nfields,
                               ps.segBaseValid ? ps.segBase : line_va);
    }
    return 0;
}

void
ViperFinal::streamAheadStrided(SttEntry &ps,
                               std::vector<AddrPriority> &addresses)
{
    // Same lookahead in BYTES OF INDEX DATA as the unit-stride window
    // (prefetchDistance lines of indices), expressed in elements and
    // scaled by the stride to a span.
    const unsigned width = ps.strideElemBytes;
    const int64_t stride = ps.stride;
    const uint64_t vl = (ps.chunkEnd - ps.chunkStart) / stride;
    const uint64_t elems_ahead = (uint64_t)prefetchDistance *
                                 ((uint64_t)blkSize / width);
    const Addr from = ps.chunkStart + elems_ahead * stride;
    const Addr to = from + vl * stride;              // exclusive
    Addr last_line = 0;
    for (Addr a = from; a < to; a += (stride < (int64_t)blkSize
                                       ? (Addr)blkSize : (Addr)stride)) {
        // stride < line: every line of the span holds elements, step
        // by line; stride >= line: step by element, one line each.
        const Addr line = blockAddress(a);
        if (line == last_line) {
            continue;
        }
        last_line = line;
        if (line >= ps.limitAddr) {
            addresses.push_back(AddrPriority(line, 0));
            viperStats.streamCandidates++;
        }
    }
    const Addr end_line = blockAddress(to - 1) + blkSize;
    if (end_line > ps.limitAddr) {
        ps.limitAddr = end_line;
    }
}

void
ViperFinal::captureStridedReadHit(SttEntry &ps, const PrefetchInfo &pfi,
                                  Addr pc, Addr addr, bool is_secure,
                                  const CacheAccessor &cache)
{
    if (captureBacklogged()) {
        viperStats.capturesGated++;
        return;
    }
    const Addr line = blockAddress(addr);
    const Addr line_pa = blockAddress(pfi.getPaddr());
    // Retired tail: this producer already converted this line (the
    // common case: after the first element's hit captures it, the
    // other vl-1 element hits on the same line land here).
    if (wasConvertedBy(line_pa, pc, is_secure)) {
        viperStats.recapturesDetected++;
        if (!captureReadHitOwnProducer) {
            viperStats.recapturesSuppressed++;
            return;
        }
    }
    if (ps.indexLineQueue.size() >= indexLineQueueEntries) {
        viperStats.bufferBusyDrops++;
        return;
    }
    CapturedLine cap;
    cap.data.resize(blkSize);
    if (!cache.readLine(line_pa, is_secure, cap.data.data(), blkSize)) {
        return; // not resident after all: the fill path will capture it
    }
    dropRegistration(line_pa, is_secure);
    cap.lineVaddr = line;
    cap.slotMask = strideSlotMask(line, ps.strideElemBytes, ps.stride,
                                  ps.chunkStart);
    ps.indexLineQueue.push_back(std::move(cap));
    viperStats.hitsCaptured++;
    viperStats.stridedHitsCaptured++;
    retireRegistration(line_pa, pc, is_secure);
    scheduleDrain();
}

void
ViperFinal::dropRegistration(Addr line_pa, bool secure)
{
    for (auto it = indexRoutingTable.begin();
         it != indexRoutingTable.end(); ++it) {
        if (it->linePaddr == line_pa && it->secure == secure) {
            indexRoutingTable.erase(it);
            return;
        }
    }
}

void
ViperFinal::retireRegistration(Addr line_pa, Addr producer_pc,
                                bool secure)
{
    if (retiredTailEntries == 0) {
        return;
    }
    for (auto it = retiredRoutingTail.begin();
         it != retiredRoutingTail.end(); ++it) {
        if (it->linePaddr == line_pa && it->producerPC == producer_pc &&
            it->secure == secure) {
            // Already recorded: move it to the back so a line being
            // converted repeatedly does not age out under its own
            // traffic, and so the tail holds one record per line.
            retiredRoutingTail.splice(retiredRoutingTail.end(),
                                      retiredRoutingTail, it);
            return;
        }
    }
    if (retiredRoutingTail.size() >= retiredTailEntries) {
        retiredRoutingTail.pop_front();
    }
    retiredRoutingTail.push_back({line_pa, producer_pc, secure});
}

bool
ViperFinal::wasConvertedBy(Addr line_pa, Addr producer_pc,
                            bool secure) const
{
    for (const RetiredEntry &r : retiredRoutingTail) {
        if (r.linePaddr == line_pa && r.producerPC == producer_pc &&
            r.secure == secure) {
            return true;
        }
    }
    return false;
}

bool
ViperFinal::captureBacklogged() const
{
    // Backlog is the whole in-flight pipeline: queued targets plus
    // targets parked awaiting an MMU translation (indirect targets are
    // nearly all cross-page, so the translation queue is real depth,
    // not a corner case).
    return captureQueueGate != 0 &&
           pfq.size() + pfqMissingTranslation.size() >= captureQueueGate;
}

// ---------------------------------------------------------------------
// Cache feedback (resident drops) and read-hit capture
// ---------------------------------------------------------------------

void
ViperFinal::pfHitInCache()
{
    Base::pfHitInCache(); // keep the stat exactly as before
    if (dropBatchConfidence == 0 || !lastIssuedValid) {
        return; // feedback off, or the drop was a stream candidate
    }
    const Addr src = lastIssuedSource;
    lastIssuedValid = false;
    viperStats.batchDropSamples++;

    // Tally the drop against its batch (small recent-batch list).
    auto rec = batchDropRecs.begin();
    while (rec != batchDropRecs.end() && rec->source != src) {
        ++rec;
    }
    if (rec == batchDropRecs.end()) {
        if (batchDropRecs.size() >= 8) {
            batchDropRecs.pop_front();
        }
        batchDropRecs.push_back({src, 0});
        rec = std::prev(batchDropRecs.end());
    }
    if (++rec->drops < dropBatchConfidence) {
        return; // not confident yet
    }
    batchDropRecs.erase(rec);

    // Confidence reached: the cache has proved this batch's source
    // line resident dropBatchConfidence times. Measured residency is
    // all-or-nothing per index line, so forfeit the siblings.
    viperStats.batchesAborted++;

    // The aborted-batch record carries the Bresenham accumulator that
    // spreads drop_batch_fraction evenly over the batch's remaining
    // targets; the queue flush below and the push-site suppression
    // share it, so the overall discard rate is exactly the fraction.
    // Bounded FIFO: an aged-out abort lets a genuine later revisit
    // re-emit, which is correct.
    auto ab = abortedBatches.begin();
    while (ab != abortedBatches.end() && ab->source != src) {
        ++ab;
    }
    if (ab == abortedBatches.end()) {
        if (abortedBatches.size() >= 8) {
            abortedBatches.pop_front();
        }
        abortedBatches.push_back({src, 0.0});
        ab = std::prev(abortedBatches.end());
    }

    // 1) Flush the batch's targets still staged in the prefetch
    //    queue (same removal idiom as Queued's pfRemovedDemand),
    //    thinned to the configured fraction - a kept target keeps its
    //    batch tag so a later resident-drop still attributes.
    //    pfqMissingTranslation cannot be edited - translationComplete
    //    asserts its entry is still listed - so targets parked there
    //    leak through and are dropped by the cache as before.
    for (auto it = pfq.begin(); it != pfq.end();) {
        const Addr t_line = blockAddress(it->pfInfo.getAddr());
        auto tag = emittedFrom.find(t_line);
        if (tag != emittedFrom.end() && tag->second == src) {
            ab->acc += dropBatchFraction;
            if (ab->acc >= 1.0) {
                ab->acc -= 1.0;
                emittedFrom.erase(tag);
                delete it->pkt;
                it = pfq.erase(it);
                viperStats.targetsFlushedOnDrop++;
                continue;
            }
        }
        ++it;
    }
    // 2) The batch's not-yet-emitted remainder is thinned at the push
    //    site (drainEmission checks abortedBatches with the same
    //    accumulator).
}

void
ViperFinal::captureFromReadHit(SttEntry &ps, const PrefetchInfo &pfi,
                                Addr pc, Addr addr, unsigned size,
                                bool is_secure)
{
    // Same admission backpressure as the registration paths
    // (capture_queue_gate): a congested queue means this batch would
    // sit in the index line queue and issue behind the demand.
    if (captureBacklogged()) {
        viperStats.capturesGated++;
        return;
    }
    // satisfyRequest copies exactly the requested range into the packet's
    // buffer (setDataFromBlock), so payload[0, bytes) maps onto
    // [addr, addr + bytes) — the same span the PrefetchInfo data covers.
    const uint8_t *payload = hitPkt->getConstPtr<uint8_t>();
    const unsigned bytes = std::min(size, hitPkt->getSize());
    for (Addr line = blockAddress(addr); line < addr + bytes;
         line += blkSize) {
        // Only whole lines inside the access carry a full chunk to
        // slice; the fill path refuses short payloads for the same
        // reason (pkt->getSize() < blkSize). A vector unit-stride index
        // load reaches the cache as line-sized accesses, so in practice
        // this keeps exactly the one line the hit covers.
        if (line < addr || line + blkSize > addr + bytes) {
            continue;
        }
        const unsigned offset = line - addr;
        // IRT keys are block-aligned PAs (notifyFill matches
        // blockAddress(pkt->getAddr())).
        const Addr line_pa = blockAddress(pfi.getPaddr() + offset);

        // The retired tail says whether THIS producer's conversion of
        // this line has already run. If it has, the hit would push the
        // identical elements through the lanes a second time - and that
        // is the common case once the streaming half is working, since
        // the walk prefetches the index line itself, the fill converts
        // it, and the demand read that follows necessarily hits. A
        // retired record left by a DIFFERENT producer is not a
        // duplicate: that is the multi-chain case this path exists for,
        // where a sibling's walk made the line resident and this
        // producer has yet to see the data at all.
        if (wasConvertedBy(line_pa, pc, is_secure)) {
            viperStats.recapturesDetected++;
            if (!captureReadHitOwnProducer) {
                viperStats.recapturesSuppressed++;
                continue;
            }
        }
        if (ps.indexLineQueue.size() >= indexLineQueueEntries) {
            viperStats.bufferBusyDrops++;
            return;
        }
        // A pending registration for this line can never latch now (the
        // line is resident, so no fill is coming) and would
        // double-convert if a later fill did arrive. Drop it.
        dropRegistration(line_pa, is_secure);

        CapturedLine cap;
        cap.data.assign(payload + offset, payload + offset + blkSize);
        cap.lineVaddr = line;
        cap.slotMask = captureSlotMask(ps, line);
        ps.indexLineQueue.push_back(std::move(cap));
        viperStats.hitsCaptured++;
        retireRegistration(line_pa, pc, is_secure);
    }
}

bool
ViperFinal::convertChunk(Addr pc, SttEntry &ps, CapturedLine &cap)
{
    // The slice width lives on the producer's IPT set. The set is
    // there: a line is queued only while its producer is live, and
    // the invalidate that frees the last way flushes the queue.
    const int si = iptFind(pc);
    assert(si >= 0);
    const unsigned width = ipt[si].elemBytes;
    // The whole captured line is index data: VIPER-FINAL does not
    // clamp the slice at an announced extent. A line is captured
    // because it sits inside a producer's own stream window, or
    // because the demand missed on it at a producer PC — either way
    // its bytes are that producer's index array by construction. The
    // announcement bounds where the WALK may go; it does not change
    // how a line already fetched is read.
    const unsigned elems = cap.data.size() / width;
    if (cap.nextElem == 0) {
        // First chunk of this line: reset the per-line bookkeeping.
        clearLineState(ps);
        // Random tail-drop throttle (tail policy): while the current
        // epoch's scale is nonzero, one draw per captured line
        // forfeits 1..floor(elems*scale) elements off the line's END
        // before any lane converts them — a randomized per-gather
        // prefetch-degree clamp. Dropped elements are never recorded
        // emitted, so a later recapture may still convert them.
        if (randomDropInterval != 0 && currentDropScale > 0.0) {
            const unsigned max_drop =
                (unsigned)(elems * currentDropScale);
            if (max_drop >= 1) {
                cap.tailDropElems =
                    (uint8_t)(1 + randomDropRng() % max_drop);
                viperStats.linesRandomTruncated++;
                viperStats.elementsRandomDropped += cap.tailDropElems;
            }
        }
    }
    // The random tail-drop throttle is the only thing that shortens
    // the effective element count.
    const unsigned eff_elems =
        elems - std::min(elems, (unsigned)cap.tailDropElems);

    const unsigned first = cap.nextElem;

    // Each way's form is read once per chunk from the IPT record,
    // which is its only copy; a way with no live form idles.
    std::vector<const Form *> forms(ps.groups.size());
    for (unsigned g = 0; g < ps.groups.size(); g++) {
        forms[g] = wayForm(pc, g);
    }
    // Way slot masks. A unit-stride line has none (0 = every slot);
    // a strided (vlse) line's grid mask is shared by every way; a
    // segment (vlseg) line carries the field-0 base mask and each
    // way reads it rotated by its own field id (fieldSlotMask) --
    // the per-way mask is derived at the head of the queue, never
    // stored. any_mask is their union: a slot no live way wants is
    // skipped before the broadcast, exactly as an off-grid vlse slot.
    const unsigned nf = ipt[si].nfields;
    std::vector<uint64_t> way_mask(ps.groups.size(), cap.slotMask);
    uint64_t any_mask = cap.slotMask;
    const bool per_field = nf >= 2 && cap.slotMask != 0;
    if (per_field) {
        any_mask = 0;
        for (unsigned g = 0; g < ps.groups.size(); g++) {
            if (forms[g] != nullptr) {
                way_mask[g] = fieldSlotMask(cap.slotMask, nf,
                                            forms[g]->field);
                any_mask |= way_mask[g];
            }
        }
    }

    ConvertedLine batch;
    batch.lineVaddr = cap.lineVaddr;
    batch.targets.reserve((eff_elems - first) * ps.groups.size());

    // One shift-and-add lane group per consumer way, all fed by this
    // one broadcast line: at conversion_lanes=0 the whole line
    // converts at once, which is the point of restricting the chain
    // to an affine form (GDP walks the same payload through its
    // replay pipeline at pipelines_per_gather elements per event
    // instead — the drain compute width is the entire structural
    // difference between the two designs). A nonzero lane budget is
    // ELEMENT slots shared by all ways, charged on any-miss.
    //
    // The seen-set compare is one CAM on folded index keys, and it
    // names target lines exactly (preLaneKey). pre_lane_dedup only
    // decides WHEN it runs relative to the lane: before it, so a hit
    // clock-gates the lane and spends no budget slot, or after it, so
    // the lane has already fired. The emitted set is identical.
    unsigned slots_used = 0;
    unsigned i = first;
    for (; i < eff_elems && (conversionLanes == 0
                             || slots_used < conversionLanes); i++) {
        // Strided producer lines: only the slots on the stride's
        // phase hold indices (the rest is interleaved payload); an
        // off-mask slot fires no lane and costs no budget. Segment
        // lines: a slot outside every live way's field likewise.
        if (any_mask != 0 && !((any_mask >> i) & 1ULL)) {
            continue;
        }
        uint64_t raw = 0;
        std::memcpy(&raw, cap.data.data() + i * width, width);
        const uint64_t idx = letoh(raw);

        bool any_fired = false;
        bool any_gated = false;
        for (unsigned g = 0; g < ps.groups.size(); g++) {
            const Form *wf = forms[g];
            if (wf == nullptr) {
                continue;
            }
            // Segment lines: this slot belongs to another field than
            // the one this way consumes -- its lane set is gated off
            // (one AND of the way's mask bit; no lane, no budget).
            if (per_field && !((way_mask[g] >> i) & 1ULL)) {
                viperStats.fieldSlotsSkipped++;
                continue;
            }
            ConsumerGroup &cg = ps.groups[g];
            const Addr key = preLaneKey(*wf, idx);
            const bool dup = seenSetDedup &&
                std::find(cg.lineSeen.begin(), cg.lineSeen.end(), key)
                != cg.lineSeen.end();
            if (dup && preLaneDedup) {
                viperStats.targetsDeduplicated++;
                any_gated = true;
                continue; // no lane circuit, no budget share
            }
            any_fired = true;
            viperStats.elementsConverted++;
            if (dup) {
                viperStats.targetsDeduplicated++;
                continue; // post-lane placement: the lane fired first
            }
            const Addr target = applyForm(*wf, idx);
            if (target == 0 || (target & 0xffffff0000000000ULL)) {
                viperStats.targetsFiltered++;
                continue; // filtered targets never enter the seen-set
            }
            if (seenSetDedup) {
                cg.lineSeen.push_back(key);
                if (seenSetEntries != 0 &&
                    cg.lineSeen.size() > seenSetEntries) {
                    cg.lineSeen.pop_front(); // FIFO: the oldest leaves
                    viperStats.seenSetReplaced++;
                }
            }
            batch.targets.push_back({target, (uint8_t)g});
        }
        // The any-miss slot rule: an element only some ways gate on
        // still costs one slot (the firing ways run in it together);
        // an element EVERY way gates on is free.
        if (any_fired) {
            slots_used++;
        } else if (any_gated) {
            viperStats.slotsCompacted++;
        }
    }
    cap.nextElem = (uint8_t)i;
    const bool done = (unsigned)cap.nextElem >= eff_elems;
    if (!done) {
        viperStats.conversionWidthLimited++;
    }

    DPRINTF(ViperFinal, "converted index line VA %#x elems [%u,%u) of %u "
            "x %u ways -> %u targets%s\n",
            cap.lineVaddr, first, i, eff_elems,
            (unsigned)ps.groups.size(),
            (unsigned)batch.targets.size(),
            done ? "" : " [width-limited]");
    if (batch.targets.empty()) {
        return done; // nothing to latch; the caller finalizes if done
    }
    batch.lastChunk = done;
    ps.latch = std::move(batch);
    return done;
}

void
ViperFinal::drainEmission(std::vector<AddrPriority> &addresses)
{
    // Emission budget: free queue slots, but never less than the floor.
    // The floor models the continuous queue drain real hardware has —
    // gem5's queue empties only on cache pulls, which starve under high
    // miss rates, and a zero budget there wedges emission outright.
    // drain_floor=0 gives the pure stall for comparison.
    int room = (int)queueSize - (int)pfq.size()
             - (int)pfqMissingTranslation.size()
             - (int)addresses.size();
    // drop_on_full replaces the drain_floor displacement license:
    // room is never forced, and overflow targets are discarded below
    // instead of pushed into a full queue (where Queued::insert
    // would evict staged entries).
    if (room < drainFloor && !dropOnFull) {
        room = drainFloor;
    }
    bool blocked = false;
    bool line_limited = false;
    for (auto &kv : streamTrackingTable) {
        if (blocked) {
            break;
        }
        const Addr pc = kv.first;
        SttEntry &ps = kv.second;
        // The lane array runs once per producer per event: it can
        // drain its output latch and convert AT MOST ONE chunk of
        // the front buffered line into it. Stale-line flushes stay
        // free — they model the broadside invalidate, not the array.
        // Nothing here tests whether the producer is live: its queue
        // and latch are non-empty only while it is (the invalidate
        // that frees its last way flushes both).
        bool converted = false;
        while (true) {
            if (ps.latch.pending()) {
                if (room <= 0 && !dropOnFull) {
                    blocked = true;
                    break; // the stall: finished addresses wait latched
                }
                while (ps.latch.pending() && (room > 0 || dropOnFull)) {
                    const ConvertedLine::TargetEntry te =
                        ps.latch.targets[ps.latch.next++];
                    ConsumerGroup &cg = ps.groups[te.group];
                    // A way invalidated between conversion and
                    // emission forfeits its own targets; the other
                    // ways keep emitting.
                    if (wayForm(pc, te.group) == nullptr) {
                        viperStats.noFormSkips++;
                        continue;
                    }
                    if (dedupBufferSize) {
                        const Addr line = blockAddress(te.addr);
                        if (inDedupWindow(cg, line)) {
                            // First-emit-wins: an earlier line inside
                            // the window already pushed this line.
                            viperStats.targetsCrossDeduplicated++;
                            continue; // no queue slot consumed
                        }
                    }
                    if (room <= 0) {
                        // drop_on_full: the queue keeps its staged
                        // entries; the overflow target is forfeited.
                        // NOT recorded emitted — no prefetch went
                        // out, so a later duplicate may re-emit.
                        viperStats.targetsDroppedFull++;
                        continue;
                    }
                    // drop_batch_confidence: a batch the cache already
                    // proved resident forfeits drop_batch_fraction of
                    // its remaining targets (Bresenham accumulator
                    // shared with the abort-time queue flush).
                    if (dropBatchConfidence != 0) {
                        auto ab = std::find_if(
                            abortedBatches.begin(), abortedBatches.end(),
                            [&](const AbortedBatch &b) {
                                return b.source == ps.latch.lineVaddr;
                            });
                        if (ab != abortedBatches.end()) {
                            ab->acc += dropBatchFraction;
                            if (ab->acc >= 1.0) {
                                ab->acc -= 1.0;
                                viperStats.targetsFlushedOnDrop++;
                                continue; // no queue slot consumed
                            }
                        }
                    }
                    // Eviction-clocked dedup: a tracked line was
                    // emitted earlier and has not been seen leaving
                    // the cache since — suppression lasts exactly as
                    // long as re-emission would be wasted, for any
                    // reuse distance (the recurrence the fixed-depth
                    // window above cannot see).
                    if (evictDedupEntries != 0 &&
                        evictDedupSuppress(blockAddress(te.addr))) {
                        viperStats.targetsEvictDeduplicated++;
                        continue; // no queue slot consumed
                    }
                    if (dedupBufferSize) {
                        cg.pendingEmitted.push_back(
                            blockAddress(te.addr));
                    }
                    if (dropBatchConfidence != 0) {
                        // Tag the target with its source batch so a
                        // resident-drop can be attributed. One-shot:
                        // the getPacket() peek consumes the tag.
                        const Addr t_line = blockAddress(te.addr);
                        if (emittedFrom.insert(
                                {t_line, ps.latch.lineVaddr}).second) {
                            emittedFromFifo.push_back(t_line);
                            if (emittedFromFifo.size() > 2 * queueSize) {
                                emittedFrom.erase(emittedFromFifo.front());
                                emittedFromFifo.pop_front();
                            }
                        }
                    }
                    // Track only targets actually pushed — an
                    // overflow drop above never went out, so a later
                    // duplicate may still re-emit it.
                    if (evictDedupEntries != 0) {
                        evictDedupInsert(blockAddress(te.addr));
                    }
                    addresses.push_back(AddrPriority(te.addr, 0));
                    viperStats.targetsGenerated++;
                    room--;
                    DPRINTF(ViperFinal, "target: %#x (way %u)\n",
                            te.addr, te.group);
                }
                if (ps.latch.pending()) {
                    blocked = true;
                    break; // stalled mid-latch; resume next access
                }
                // Fully emitted. Each way's window entry closes with
                // the line's last chunk.
                if (ps.latch.lastChunk) {
                    closeLineDedup(ps);
                }
                continue;
            }
            // Latch free: convert the next buffered line into it.
            if (ps.indexLineQueue.empty()) {
                break;
            }
            if (converted) {
                line_limited = true;
                break; // the array already ran this event
            }
            CapturedLine &cap = ps.indexLineQueue.front();
            // The lane groups: up to conversion_lanes element slots
            // per event across all ways; a wider line stays at the
            // buffer front and resumes next event.
            const bool line_done = convertChunk(pc, ps, cap);
            if (line_done) {
                ps.indexLineQueue.pop_front();
                if (!ps.latch.pending()) {
                    // Line finished without latching anything (all
                    // filtered/deduped): close its dedup entries here.
                    closeLineDedup(ps);
                }
            }
            converted = true;
        }
    }
    if (blocked) {
        viperStats.emissionDeferred++;
    }
    if (line_limited) {
        viperStats.emissionLineLimited++;
    }
}

// ---------------------------------------------------------------------
// Self-clocked drain
// ---------------------------------------------------------------------

bool
ViperFinal::drainWorkPending() const
{
    for (const auto &kv : streamTrackingTable) {
        const SttEntry &ps = kv.second;
        if (ps.latch.pending() || !ps.indexLineQueue.empty()) {
            return true;
        }
    }
    return false;
}

void
ViperFinal::scheduleDrain()
{
    if (drainEvent.scheduled() || !drainWorkPending()) {
        return;
    }
    schedule(drainEvent, clockEdge(Cycles(1)));
}

void
ViperFinal::drainTick()
{
    // No context until the first demand access has been observed; the
    // next notify() re-arms.
    if (!drainCtxPfi || !drainCtxCache) {
        return;
    }
    // Wait for genuine queue room rather than forcing the floor: the
    // floor exists because demand events are too scarce to waste, but
    // ticks are plentiful — overshooting the queue just churns drops.
    // Not re-armed here on purpose: room only appears through a cache
    // pull (getPacket) or a demand event (notify), and both re-arm.
    const int room = (int)queueSize - (int)pfq.size()
                   - (int)pfqMissingTranslation.size();
    if (room < std::max(drainFloor, 1)) {
        return;
    }
    viperStats.selfDrainTicks++;
    std::vector<AddrPriority> addresses;
    drainEmission(addresses);
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
    // Start any pending cross-page translations now. Stock Queued
    // only starts them from getPacket(), which the cache calls only
    // when pfq already holds a ready entry: with every candidate
    // cross-page (lookahead >= a page, i.e. prefetch_distance >= 64
    // lines; the chunk-era VLEN=8192 pd=4 cell) pfq stays empty, the translation queue
    // fills and evicts forever, and nothing ever issues (the ReVeLA
    // wedge, fixed there 2026-08-12; found here 2026-09-11).
    if (!pfqMissingTranslation.empty()) {
        processMissingTranslations(queueSize - pfq.size());
    }
    scheduleDrain();
}

// ---------------------------------------------------------------------
// Access, issue and fill hooks
// ---------------------------------------------------------------------

void
ViperFinal::notify(const CacheAccessProbeArg &acc, const PrefetchInfo &pfi)
{
    // Latch this access's translation context for the self-clocked
    // drain. The request must carry what insert() dereferences: a VA
    // (useVirtualAddresses PA math) and a ContextID (cross-page
    // translation requests). The Request is a shared_ptr, so it
    // outlives the packet safely.
    if (acc.pkt != nullptr && acc.pkt->req != nullptr &&
        acc.pkt->req->hasVaddr() && acc.pkt->req->hasContextId()) {
        drainCtxReq = acc.pkt->req;
        drainCtxPfi.reset(new PrefetchInfo(pfi, pfi.getAddr()));
        drainCtxCache = &acc.cache;
    }
    // Lend calculatePrefetch the read port's payload for the duration of
    // this call. A satisfied demand-read hit already had the block copied
    // into its packet by access()->setDataFromBlock, which runs BEFORE the
    // Hit probe fires (mem/cache/base.cc), so the bytes are readable here.
    // The same predicate the core uses to decide a read hit carries data
    // (PrefetchInfo's ctor, mem/cache/prefetch/base.cc): HW-prefetch reads
    // have no CPU-allocated buffer and must be excluded.
    if (captureOnReadHit && acc.pkt != nullptr && !pfi.isCacheMiss() &&
        acc.pkt->isRead() && !acc.pkt->cmd.isHWPrefetch() && pfi.hasData()) {
        hitPkt = acc.pkt;
    }
    // Eviction-clocked dedup: the demand stream is the second truth
    // source. A hit to a tracked line refreshes it (recurring AND
    // resident — the entries worth keeping at capacity); a miss
    // proves it absent and re-arms it immediately.
    if (evictDedupEntries != 0) {
        evictDedupObserveAccess(blockAddress(pfi.getAddr()),
                                pfi.isCacheMiss());
    }
    // Random tail-drop epoch clock: every random_drop_interval
    // observed accesses, redraw the epoch's drop scale uniformly from
    // the candidate set. Demand-paced on purpose — a cycle clock
    // would stretch epochs under exactly the congestion being probed.
    if (randomDropInterval != 0) {
        // Geometric mode: memoryless epoch lengths — each access
        // redraws with probability 1/interval (same mean epoch), so
        // the clock cannot phase-lock with loop periodicity. The
        // min-interval refractory floor shifts the distribution
        // (epoch = min + Geometric(1/interval)): rolls only resume
        // once the epoch is min accesses old, so the geometric's
        // short-epoch mass — drop epochs too brief to matter — is
        // excised while the memoryless tail survives.
        const bool redraw = randomDropGeometric
            ? (++notifiesSinceDraw >= randomDropMinInterval &&
               randomDropRng() % randomDropInterval == 0)
            : (++notifiesSinceDraw >= randomDropInterval);
        if (redraw) {
            notifiesSinceDraw = 0;
            viperStats.epochRedraws++;
            currentDropScale = randomDropRates[
                randomDropRng() % randomDropRates.size()];
        }
    }
    Queued::notify(acc, pfi);
    hitPkt = nullptr;
    // Same kick as drainTick: this covers the inserts made by this
    // very notify.
    if (!pfqMissingTranslation.empty()) {
        processMissingTranslations(queueSize - pfq.size());
    }
    scheduleDrain();
}

void
ViperFinal::evictDedupErase(std::list<EvictDedupEntry>::iterator it)
{
    evictDedupByVa.erase(it->lineVaddr);
    if (it->paValid) {
        auto pa = evictDedupByPa.find(it->linePaddr);
        if (pa != evictDedupByPa.end() && pa->second == it) {
            evictDedupByPa.erase(pa);
        }
    }
    evictDedupLru.erase(it);
}

bool
ViperFinal::evictDedupSuppress(Addr line_va)
{
    auto it = evictDedupByVa.find(line_va);
    if (it == evictDedupByVa.end()) {
        return false;
    }
    // A suppression is recurrence evidence: refresh the entry.
    evictDedupLru.splice(evictDedupLru.begin(), evictDedupLru,
                         it->second);
    return true;
}

void
ViperFinal::evictDedupInsert(Addr line_va)
{
    auto it = evictDedupByVa.find(line_va);
    if (it != evictDedupByVa.end()) {
        evictDedupLru.splice(evictDedupLru.begin(), evictDedupLru,
                             it->second);
        return;
    }
    while (evictDedupLru.size() >= evictDedupEntries) {
        // Replacement is fail-safe: a forgotten line re-emits once
        // and the cache squashes it if it was still resident.
        viperStats.evictDedupReplaced++;
        evictDedupErase(std::prev(evictDedupLru.end()));
    }
    evictDedupLru.push_front({line_va});
    evictDedupByVa[line_va] = evictDedupLru.begin();
}

void
ViperFinal::evictDedupBindPa(Addr line_va, Addr line_pa)
{
    auto it = evictDedupByVa.find(line_va);
    if (it == evictDedupByVa.end()) {
        return;
    }
    // The PA index maps one physical line to at most one entry; a
    // rebind (the line re-emitted after an unseen eviction and
    // re-translated) just moves the mapping to the fresh entry.
    if (it->second->paValid) {
        auto old = evictDedupByPa.find(it->second->linePaddr);
        if (old != evictDedupByPa.end() && old->second == it->second) {
            evictDedupByPa.erase(old);
        }
    }
    it->second->linePaddr = line_pa;
    it->second->paValid = true;
    evictDedupByPa[line_pa] = it->second;
}

void
ViperFinal::evictDedupObserveAccess(Addr line_va, bool miss)
{
    auto it = evictDedupByVa.find(line_va);
    if (it == evictDedupByVa.end()) {
        return;
    }
    if (miss) {
        // The line is provably not resident: re-arm now. This heals
        // entries whose eviction the PA index never saw (a prefetch
        // dropped at translation, or a PA bound after the line
        // already left), at the cost of the one miss just paid.
        viperStats.evictDedupMissCleared++;
        evictDedupErase(it->second);
    } else {
        // The line is provably resident and recurring: refresh.
        evictDedupLru.splice(evictDedupLru.begin(), evictDedupLru,
                             it->second);
    }
}

void
ViperFinal::notifyEvict(const EvictionInfo &info)
{
    const Addr line_pa = blockAddress(info.addr);
    if (evictDedupEntries != 0 && !evictDedupByPa.empty()) {
        auto it = evictDedupByPa.find(line_pa);
        if (it != evictDedupByPa.end()) {
            viperStats.evictDedupEvictCleared++;
            evictDedupErase(it->second);
        }
    }
}

void
ViperFinal::calculatePrefetch(const PrefetchInfo &pfi,
    std::vector<AddrPriority> &addresses,
    const CacheAccessor &cache)
{
    if (pfi.isWrite() || !pfi.hasPC()) {
        // No chunk-event processing, but emission still runs: any
        // observed access carries the translation context it needs.
        drainEmission(addresses);
        return;
    }

    const Addr addr = pfi.getAddr();
    const unsigned size = pfi.getSize();
    const Addr pc = pfi.getPC();
    const bool is_secure = pfi.isSecure();

    // The producer query: is this PC a unit-stride vector load?
    // Nothing else is read from the chain table here — whether the
    // producer has a form, which, and at what slice width, is what
    // the IPT already holds.
    const VectorChainTable::ProducerInfo info = tbl->producerInfo(pc);
    // Unit-stride index loads only. A strided (vlse) producer issues
    // one access per ELEMENT, so treating it as unit-stride would give
    // every element its own chunk event and its own walk -- a window
    // placed prefetch_distance lines ahead of a cursor that
    // advances by the stride. The chain table still reports these
    // heads (ProducerInfo::stride, cpu/vector_chain_table.cc), so they
    // have to be rejected HERE; VIPER-FINAL has no span model for
    // them.
    if (!info.found || (info.stride != 0 && !stridedProducers)) {
        drainEmission(addresses);
        return;
    }
    // Segment (vlseg) producers (2026-09-16): memory-side a plain
    // unit-stride stream (the ISA issues contiguous requests over the
    // interleaved region), so the chunk/walk model below applies as
    // is; what differs is that a captured line interleaves nf fields
    // and each consumer way converts only its field's slots. Admission
    // is the slot model's: EEW dividing the line, at most 64 slots.
    if (info.nfields >= 2 &&
        (!segmentProducers || size == 0 || blkSize % size != 0 ||
         (blkSize / size) > 64)) {
        viperStats.segmentUnsupported++;
        drainEmission(addresses);
        return;
    }
    // Strided (vlse) producers (2026-09-12): one EEW-byte access per
    // element; a chunk is the span of vl elements at `stride` bytes.
    // Admission: the element grid must be EEW-aligned inside a line
    // (stride a positive multiple of EEW, EEW dividing the line), so
    // every element maps to one slot of the captured line.
    const bool strided = info.stride != 0;
    if (strided) {
        const int64_t st = info.stride;
        if (st < (int64_t)size || st % (int64_t)size != 0 ||
            blkSize % size != 0 || size == 0 ||
            (blkSize / size) > 64) {
            viperStats.stridedUnsupported++;
            drainEmission(addresses);
            return;
        }
    }

    // An STT row exists from its first chunk event on; that presence
    // is its validity.
    auto ins = streamTrackingTable.try_emplace(pc);
    SttEntry &ps = ins.first->second;
    const bool fresh = ins.second;
    if (fresh) {
        // The consumer ways are physical: one dedup record per way,
        // sized once to the array width.
        ps.groups.resize(consumersPerProducer);
    }
    // Segment producer state, refreshed every access: the field count
    // is static, the base moves every strip (its phase does not).
    ps.nfields = info.nfields;
    ps.segElemBytes = (uint8_t)info.elemBytes;
    ps.segBaseValid = info.segBaseValid;
    ps.segBase = info.segBase;

    if (strided) {
        const int64_t st = info.stride;
        // vl is captured at the producer's dispatch (VectorChainTable
        // DCT row), so it is normally set by the time an element
        // access arrives; the fallback is one line of elements (the
        // prefetcher no longer knows VLEN).
        const uint64_t vl = info.vl != 0 ? info.vl : (blkSize / size);
        const Addr span = (Addr)vl * (Addr)st;
        // Same chunk: any element access inside the current span, or
        // up to one span BEHIND its recorded start (element micro-ops
        // may complete out of order, so the first access seen is not
        // always element 0; an earlier element just moves the start
        // back on the same grid).
        const bool same_span = !fresh && ps.stride == st &&
            addr < ps.chunkEnd && addr + span > ps.chunkStart;
        if (same_span) {
            if (addr < ps.chunkStart) {
                ps.chunkStart = addr;
                ps.chunkEnd = addr + span;
            }
        } else {
            viperStats.chunksObserved++;
            viperStats.stridedChunks++;
            if (!fresh && addr < ps.chunkStart) {
                // a genuine backward jump: restart the walk
                ps.limitAddr = 0;
                for (auto &cg : ps.groups) {
                    cg.dedupWindow.clear();
                }
                if (seenSetEntries != 0) {
                    clearSeenSets(ps);
                }
            }
            ps.stride = st;
            ps.strideElemBytes = size;
            ps.stridePhase = (unsigned)(addr % (Addr)st);
            ps.chunkStart = addr;
            ps.chunkEnd = addr + span;
            ps.demandAddr = addr;
            // Chain-only trigger: a vlse that feeds no gather (sssp's
            // weight load, for one) must not stream — walking every
            // vlse prefetched the wrong rows (0.98x, 2026-08-29). The
            // proof that this producer feeds a gather is a live IPT
            // form (producerLive), not the chain table's link bit:
            // the DCT is small and clears every strip on sssp, so at
            // element-access time the head row has been re-inserted
            // and not yet re-linked by the strip's gather, while the
            // form pushed by the previous strip persists.
            // limit_gate extents are vl*EEW bytes, not vl*stride, so
            // the announced limit is not applied to strided walks.
            if (info.linked || producerLive(pc)) {
                streamAheadStrided(ps, addresses);
            } else {
                viperStats.stridedWalkGated++;
            }
        }
        if (captureOnReadHit && hitPkt != nullptr && !pfi.isCacheMiss() &&
            producerLive(pc)) {
            captureStridedReadHit(ps, pfi, pc, addr, is_secure, cache);
        }
        drainEmission(addresses);
        return;
    }

    // Chunk detection: one line-sized access per chunk.
    const unsigned chunk_bytes = size;
    const bool new_chunk = fresh || addr != ps.demandAddr;
    if (new_chunk) {
        viperStats.chunksObserved++;
        bool restart = !fresh && addr < ps.demandAddr;
        // Segment producers: one macro is nf memory micros of vl*EEW
        // bytes each, back to back, and the LSQ issues their line
        // fragments out of order (measured on seg1: half the index
        // lines failed registration because every reordered fragment
        // zeroed the high-water mark). A step back that stays inside
        // the macro's span is that reordering, not a restarted walk:
        // the cursor and the window stand. Unit-stride (nf <= 1)
        // producers keep the plain rule, bit-for-bit.
        bool reordered = false;
        if (restart && ps.nfields >= 2) {
            const Addr vl = info.vl != 0 ? info.vl : (blkSize / size);
            const Addr span = vl * (Addr)ps.nfields * (Addr)ps.segElemBytes;
            if (addr + span > ps.demandAddr) {
                restart = false;
                reordered = true;
                viperStats.segmentReorderTolerated++;
            }
        }
        // A jump backwards is a restarted walk: re-prefetch. The dedup
        // windows clear with it — their lines may have been evicted
        // since, and suppressing their re-emission would punch holes
        // in the restarted pass.
        if (restart) {
            viperStats.walkRestarts++;
            ps.limitAddr = 0;
            for (auto &cg : ps.groups) {
                cg.dedupWindow.clear();
            }
            // A persistent seen-set is a residency bet like the
            // window: the walk restarted, so it re-arms with it.
            if (seenSetEntries != 0) {
                clearSeenSets(ps);
            }
        }
        // A reordered fragment keeps the (higher) cursor: it is what
        // the capture window is measured from. Its walk lands below
        // the high-water mark and emits nothing.
        if (!reordered) {
            ps.demandAddr = addr;
        }
        DPRINTF(ViperFinal, "chunk: PC %#x addr %#x size %u nf %u%s%s "
                "cursor %#x limit %#x\n", pc, addr, size,
                (unsigned)ps.nfields, restart ? " restart" : "",
                reordered ? " reordered" : "", ps.demandAddr,
                ps.limitAddr);

        // Architectural stream (no confidence: the opcode said
        // unit-stride).
        streamAhead(ps, addr, chunk_bytes, announcedLimit(pc, addr),
                    addresses);
    }

    // Read-hit capture path (capture_on_read_hit). An index line that is
    // already resident produces no fill, so the registration taken for it
    // can never latch — and on a multi-chain loop that is the common case,
    // because each producer's stream walk covers its siblings' slices and
    // makes their index lines resident before their own demands arrive.
    // The read that hits carries the payload, so take the capture here
    // instead of waiting for a fill that will never come.
    if (captureOnReadHit && hitPkt != nullptr && !pfi.isCacheMiss() &&
        producerLive(pc)) {
        captureFromReadHit(ps, pfi, pc, addr, size, is_secure);
    }

    // Emit converted targets after the stream's own candidates.
    drainEmission(addresses);
}

PacketPtr
ViperFinal::getPacket()
{
    // Range-based capture: a prefetch issuing inside a live producer's
    // stream window is index data — learn the PA that notifyFill will
    // see. The issued packet's request is PA-only (createPkt,
    // queued.cc), so peek the DeferredPacket BEFORE delegating: its
    // PrefetchInfo still holds the VA the candidate was generated
    // with, and its pkt holds the translated PA.
    lastIssuedValid = false;
    if (!pfq.empty() && pfq.front().pkt != nullptr) {
        const DeferredPacket &dp = pfq.front();
        const Addr line_va = blockAddress(dp.pfInfo.getAddr());
        // drop_batch_confidence: consume this target's batch tag. The
        // cache's residency check runs synchronously after this call
        // returns, so if the packet is dropped, pfHitInCache() below
        // can attribute the drop to this batch. Stream candidates
        // carry no tag and stay invisible to the feedback.
        if (dropBatchConfidence != 0) {
            auto tag = emittedFrom.find(line_va);
            if (tag != emittedFrom.end()) {
                lastIssuedValid = true;
                lastIssuedSource = tag->second;
                emittedFrom.erase(tag);
            }
        }
        const Addr line_pa = blockAddress(dp.pkt->getAddr());
        const bool secure = dp.pfInfo.isSecure();
        // Eviction-clocked dedup: this is the one point holding a
        // target's VA and translated PA together — bind the PA so
        // notifyEvict can find the entry. Stream candidates and
        // untracked targets fall through as a no-op.
        if (evictDedupEntries != 0) {
            evictDedupBindPa(line_va, line_pa);
        }
        for (auto &kv : streamTrackingTable) {
            SttEntry &ps = kv.second;
            const Addr win_lo = blockAddress(ps.demandAddr) + blkSize;
            if (line_va < win_lo || line_va >= ps.limitAddr) {
                DPRINTF(ViperFinal, "issue: line %#x outside producer %#x "
                        "window [%#x, %#x)\n", line_va, kv.first, win_lo,
                        ps.limitAddr);
                continue;
            }
            if (!producerLive(kv.first)) {
                // A window match on an entry that cannot capture.
                // EVERY unit-stride vle owns a stream window, not
                // just the producers, so an ordinary vle can cover
                // this line too. Keep scanning: stopping here would
                // let hash-order visit order shadow the producer
                // whose window also covers it, and the line would go
                // out as a plain stream prefetch.
                continue;
            }
            // Admission backpressure (capture_queue_gate): with the
            // queue saturated, emission is stalled and the demand
            // cursor will pass this line before its batch can issue,
            // so its targets would go out behind the demand. The
            // stream prefetch itself still departs; only the
            // capture/convert half sheds load.
            if (captureBacklogged()) {
                viperStats.capturesGated++;
            } else {
                const uint64_t mask = captureSlotMask(ps, line_va);
                registerCapture(line_pa, line_va, kv.first, secure, mask);
                DPRINTF(ViperFinal, "issue: line %#x registered for "
                        "producer %#x mask %#x\n", line_va, kv.first, mask);
            }
            break;
        }
    }
    PacketPtr pkt = Queued::getPacket();
    // A pull frees queue room — the room-gated drain can run again.
    scheduleDrain();
    return pkt;
}

void
ViperFinal::notifyFill(const CacheAccessProbeArg &acc)
{
    if (indexRoutingTable.empty()) {
        return;
    }
    const PacketPtr pkt = acc.pkt;
    if (!pkt->hasData()) {
        return; // upgrades and whole-line writes carry no payload
    }
    const Addr line_pa = blockAddress(pkt->getAddr());
    auto it = indexRoutingTable.begin();
    while (it != indexRoutingTable.end() &&
           !(it->linePaddr == line_pa && it->secure == pkt->isSecure())) {
        ++it;
    }
    if (it == indexRoutingTable.end()) {
        return;
    }
    const IrtEntry rec = *it;
    // Leaves the pending IRT either way; whether it lands in the retired
    // tail depends on the conversion below actually running. A fill
    // dropped for want of a form, or on a full index line queue,
    // converted nothing, so a later read hit on this line is still real
    // work.
    indexRoutingTable.erase(it);

    auto ps_it = streamTrackingTable.find(rec.producerPC);
    if (ps_it == streamTrackingTable.end()) {
        return;
    }
    SttEntry &ps = ps_it->second;
    // A producer holding no form cannot convert this line. Forms are
    // dropped by invalidate pulses (a table clear, or an operand one
    // was folded from changing), so this is a plain occupancy test —
    // there is no version to compare.
    if (!producerLive(rec.producerPC)) {
        viperStats.noFormSkips++;
        return;
    }
    if (pkt->getSize() < blkSize) {
        return;
    }

    // Chunk-granular load shedding: a fill arriving with the index
    // line queue full is dropped whole, because one uncovered line stalls
    // the gather anyway.
    if (ps.indexLineQueue.size() >= indexLineQueueEntries) {
        viperStats.bufferBusyDrops++;
        return;
    }

    // Latch the raw payload, exactly as GDP does; conversion happens
    // at the drain, one line per event through the lane array.
    CapturedLine cap;
    const uint8_t *data = pkt->getConstPtr<uint8_t>();
    cap.data.assign(data, data + pkt->getSize());
    cap.lineVaddr = blockAddress(rec.lineVaddr);
    cap.slotMask = rec.slotMask;
    ps.indexLineQueue.push_back(std::move(cap));
    viperStats.fillsCaptured++;
    retireRegistration(line_pa, rec.producerPC, pkt->isSecure());
    DPRINTF(ViperFinal, "fill captured: index line VA %#x (producer %#x)\n",
            rec.lineVaddr, rec.producerPC);
    // Fresh conversion work: arm the self-clocked drain.
    scheduleDrain();
}

} // namespace prefetch
} // namespace gem5
