/**
 * Viper (VIPER) implementation. See viper.hh for the
 * design and cpu/vector_chain_table.hh for the CPU-side half it shares with GDP.
 */

#include "mem/cache/prefetch/viper.hh"

#include <algorithm>
#include <cstring>
#include <iterator>

#include "base/bitfield.hh"
#include "base/intmath.hh"
#include "base/logging.hh"
#include "base/trace.hh"
#include "debug/Viper.hh"
#include "params/ViperPrefetcher.hh"
#include "sim/byteswap.hh"

namespace gem5
{

namespace prefetch
{

Viper::Viper(const ViperPrefetcherParams &p)
  : Queued(p),
    tbl(p.link_table),
    announceTbl(p.stream_table),
    limitGate(p.limit_gate),
    streamingDistance(p.streaming_distance),
    streamStartAtDistance(p.stream_start_at_distance),
    prefetchDistance(p.prefetch_distance),
    vlenBytes(p.vlen / 8),
    streamOnly(p.stream_only),
    sliceBufferEntries(p.slice_buffer_entries),
    pipelines(p.pipelines),
    irtEntries(p.routing_entries),
    captureOnReadHit(p.capture_on_read_hit),
    retiredTailEntries(p.retired_tail_entries),
    captureReadHitOwnProducer(p.capture_read_hit_own_producer),
    captureQueueGate(p.capture_queue_gate),
    dropBatchConfidence(p.drop_batch_confidence),
    dropBatchFraction(p.drop_batch_fraction),
    residencyIntervalRegs(p.residency_intervals),
    residencyConfThreshold(p.residency_conf_threshold),
    residencyGapLines(p.residency_gap_lines),
    drainFloor(p.drain_floor),
    dedupBufferSize(p.dedup_buffer_size),
    conversionLanes(p.conversion_lanes),
    dropOnFull(p.drop_on_full),
    vlWindowDedup(p.vl_window_dedup),
    vlWindowBytes(p.vl_window_bytes),
    vlWindowShift(isPowerOf2(p.vl_window_bytes)
                  ? floorLog2(p.vl_window_bytes) : 0),
    preLaneDedup(p.pre_lane_dedup),
    consumersPerProducer(p.consumers_per_producer),
    rowScheduleBits(p.row_schedule_bits),
    reorderWindowSize(p.reorder_window_size),
    streamTrackingTable(),
    indexRoutingTable(),
    viperStats(this),
    drainEvent([this] { drainTick(); }, name())
{
    fatal_if(tbl == nullptr, "%s: no link_table set. Viper needs the "
             "VectorChainTable that is also attached to the CPU's "
             "vector_chain_table param (the config script wires both).", name());
    fatal_if(limitGate && announceTbl == nullptr,
             "%s: limit_gate needs the RevelaStreamTable announcement "
             "sideband (stream_table; the config script wires it when "
             "the gate is enabled).", name());
    fatal_if(streamingDistance < 1, "streaming_distance must be >= 1");
    fatal_if(prefetchDistance > 0 && vlenBytes == 0,
             "%s: prefetch_distance needs vlen (bits; the config script "
             "wires it from --vlen)", name());
    fatal_if(sliceBufferEntries < 1, "slice_buffer_entries must be >= 1");
    fatal_if(pipelines < 1, "pipelines must be >= 1");
    fatal_if(drainFloor < 0, "drain_floor must be >= 0");
    fatal_if(vlWindowDedup && !isPowerOf2(vlWindowBytes),
             "vl_window_bytes must be a power of two");
    fatal_if(vlWindowDedup && vlWindowBytes < blkSize,
             "vl_window_bytes must be >= the cache line size");
    fatal_if(consumersPerProducer < 1 || consumersPerProducer > 255,
             "consumers_per_producer must be in [1, 255]");
    fatal_if(dropBatchConfidence != 0 &&
             (dropBatchFraction <= 0.0 || dropBatchFraction > 1.0),
             "drop_batch_fraction must be in (0, 1]");
    fatal_if(residencyIntervalRegs != 0 && residencyConfThreshold < 1,
             "residency_conf_threshold must be >= 1");
    residencyIntervals.resize(residencyIntervalRegs);
}

void
Viper::resetLearnedState()
{
    // The VectorChainTable registers its own reset callback; only this
    // prefetcher's runtime state is cleared here.
    streamTrackingTable.clear();
    configuredCount = 0;
    indexRoutingTable.clear();
    residencyIntervals.assign(residencyIntervalRegs, {});
    lastIssuedTargetValid = false;
}

Viper::ViperStats::ViperStats(statistics::Group *parent)
  : statistics::Group(parent),
    ADD_STAT(chunksObserved, statistics::units::Count::get(),
        "chunk events at producer PCs"),
    ADD_STAT(streamCandidates, statistics::units::Count::get(),
        "index-array stream candidates emitted"),
    ADD_STAT(streamLimitClamped, statistics::units::Count::get(),
        "walk lines suppressed at the announced extent end "
        "(limit_gate)"),
    ADD_STAT(formsAdopted, statistics::units::Count::get(),
        "linear forms adopted at trigger (chain walk + fold ran)"),
    ADD_STAT(formsMemoHits, statistics::units::Count::get(),
        "triggers whose compiled form was still valid (walk skipped)"),
    ADD_STAT(chainNotReady, statistics::units::Count::get(),
        "linked triggers whose chain was not yet snoopable"),
    ADD_STAT(chainsRejected, statistics::units::Count::get(),
        "chains rejected: not collapsible to base + (idx << shift)"),
    ADD_STAT(pipelinesSaturated, statistics::units::Count::get(),
        "linked producers denied a pipeline (pipelines cap)"),
    ADD_STAT(capturesRegistered, statistics::units::Count::get(),
        "index lines registered for capture (a departing stream "
        "prefetch inside a configured producer's window)"),
    ADD_STAT(fillsCaptured, statistics::units::Count::get(),
        "captured index-line fills latched into the slice buffer"),
    ADD_STAT(hitsCaptured, statistics::units::Count::get(),
        "resident index lines latched off the read port on a demand "
        "read hit (capture_on_read_hit)"),
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
    ADD_STAT(residencyDropsTrained, statistics::units::Count::get(),
        "resident-drops consumed as residency-interval training"),
    ADD_STAT(residencyIntervalAllocs, statistics::units::Count::get(),
        "residency intervals allocated for an uncovered drop"),
    ADD_STAT(residencyIntervalTrims, statistics::units::Count::get(),
        "residency intervals trimmed/split by an observed demand miss"),
    ADD_STAT(targetsResidencySuppressed, statistics::units::Count::get(),
        "targets suppressed at emission: inside a confident residency "
        "interval"),
    ADD_STAT(bufferBusyDrops, statistics::units::Count::get(),
        "fills dropped with the slice buffer full"),
    ADD_STAT(staleConfigs, statistics::units::Count::get(),
        "fills dropped on a stale form (table cleared)"),
    ADD_STAT(elementsConverted, statistics::units::Count::get(),
        "elements pushed through a shift-and-add lane"),
    ADD_STAT(elementsBeyondLimit, statistics::units::Count::get(),
        "elements not converted: beyond the announced extent "
        "(limit_gate)"),
    ADD_STAT(elementsPreDeduped, statistics::units::Count::get(),
        "elements dropped by the pre-lane truncated-index compare "
        "(consumed no shift/add lane and no lane-budget slot)"),
    ADD_STAT(conversionWidthLimited, statistics::units::Count::get(),
        "conversion events cut short by the lane budget (the line "
        "resumed on a later event)"),
    ADD_STAT(slotsCompacted, statistics::units::Count::get(),
        "elements compacted out of the lane budget: every configured "
        "way's pre-lane CAM hit, so no slot was spent"),
    ADD_STAT(targetsGenerated, statistics::units::Count::get(),
        "indirect target candidates generated"),
    ADD_STAT(targetsFiltered, statistics::units::Count::get(),
        "targets dropped by the canonical-address filter"),
    ADD_STAT(targetsDeduplicated, statistics::units::Count::get(),
        "targets dropped as duplicate lines within one batch"),
    ADD_STAT(targetsCrossDeduplicated, statistics::units::Count::get(),
        "targets dropped: line emitted within the dedup window"),
    ADD_STAT(targetsDroppedFull, statistics::units::Count::get(),
        "targets dropped at a full queue (drop_on_full) instead of "
        "displacing queued entries"),
    ADD_STAT(stalenessAborts, statistics::units::Count::get(),
        "batches flushed: walk cursor passed their index line"),
    ADD_STAT(emissionDeferred, statistics::units::Count::get(),
        "emission events paused by a full queue (the stall firing)"),
    ADD_STAT(emissionLineLimited, statistics::units::Count::get(),
        "events where the array's one-line conversion slot ended "
        "with further lines still buffered"),
    ADD_STAT(selfDrainTicks, statistics::units::Count::get(),
        "self-clocked drain firings that ran the emission engine"),
    ADD_STAT(rowPromotions, statistics::units::Count::get(),
        "issues where a queued row-mate was promoted to the head"),
    ADD_STAT(rowScheduleMisses, statistics::units::Count::get(),
        "issues where the queue held no due row-mate to promote")
{
}

// ---------------------------------------------------------------------
// The collapse: chain -> base + (index << shift)
// ---------------------------------------------------------------------

Viper::LinearForm
Viper::collapse(const VectorChainTable::ChainSnapshot &snap) const
{
    using VOp = VectorChainTable::VOp;
    LinearForm f;
    f.gen = snap.gen;
    f.opGen = snap.opGen;
    if (!snap.valid) {
        return f;
    }

    // Running affine form of the element value: (elem << shift) + bias.
    // `bias` wraps at 64 bits exactly as the vector ALU would.
    unsigned shift = 0;
    uint64_t bias = 0;
    bool scaled = false; // any shift or bias applied yet
    bool extended = false;

    for (const auto &s : snap.ops) {
        switch (s.op) {
          case VOp::SExt:
          case VOp::ZExt:
            // The extension is the slice's own width/sign, not a
            // pipeline stage — but only while it still describes how
            // the element is READ. Once anything has scaled or biased
            // the value, an extend is a genuine mid-chain operation
            // and the affine form no longer holds. Compilers emit at
            // most one, so a second is rejected rather than composed.
            if (scaled || extended) {
                return LinearForm();
            }
            extended = true;
            f.extBits = s.extFromBits;
            f.extSigned = (s.op == VOp::SExt);
            break;

          case VOp::WMul:
          case VOp::WMulU: {
            // A fused extend-and-multiply (clang's A[B[i]] shape,
            // where gcc emits vsext.vf2 + vsll). It collapses to
            // exactly the (extension, shift) pair those two produce,
            // provided the multiplier is a power of two — anything
            // else needs a real multiplier, the pipeline this design
            // drops. Like a bare extend it must still describe how the
            // element is READ, so it cannot follow a scale or bias.
            if (scaled || extended) {
                return LinearForm();
            }
            if (s.scalar == 0 || !isPowerOf2(s.scalar)) {
                return LinearForm();
            }
            const unsigned k = ctz64(s.scalar);
            if (shift + k > 63) {
                return LinearForm();
            }
            extended = true;
            f.extBits = s.extFromBits;
            f.extSigned = (s.op == VOp::WMul);
            shift += k;
            bias *= s.scalar;   // bias is 0 here (must be leading)
            scaled = true;
            break;
          }

          case VOp::Sll: {
            const unsigned k = s.scalar & 0x3f;
            if (shift + k > 63) {
                return LinearForm();
            }
            shift += k;
            bias <<= k;
            scaled = true;
            break;
          }

          case VOp::Mul: {
            // A power-of-2 multiply IS a shift; anything else needs a
            // multiplier, which is the pipeline this design drops.
            if (s.scalar == 0 || !isPowerOf2(s.scalar)) {
                return LinearForm();
            }
            const unsigned k = ctz64(s.scalar);
            if (shift + k > 63) {
                return LinearForm();
            }
            shift += k;
            bias *= s.scalar;
            scaled = true;
            break;
          }

          case VOp::Add:
            bias += s.scalar;
            scaled = true;
            break;

          case VOp::Sub:
            bias -= s.scalar;
            scaled = true;
            break;

          // Rsub negates the element (coefficient -1), right shifts
          // discard low bits, and the logic ops are not affine at all.
          default:
            return LinearForm();
        }
    }

    // Base and bias are both constants: one adder, folded once.
    f.base = snap.base + bias;
    f.shift = shift;
    f.valid = true;
    return f;
}

uint64_t
Viper::extendRaw(const LinearForm &f, uint64_t raw) const
{
    if (f.extBits > 0 && f.extBits < 64) {
        if (f.extSigned) {
            const unsigned sh = 64 - f.extBits;
            raw = (uint64_t)((int64_t)(raw << sh) >> sh);
        } else {
            raw &= (1ULL << f.extBits) - 1;
        }
    }
    return raw;
}

Addr
Viper::applyForm(const LinearForm &f, uint64_t raw) const
{
    // The lane: one shifter, one adder.
    return f.base + (extendRaw(f, raw) << f.shift);
}

Addr
Viper::preLaneKey(const LinearForm &f, uint64_t raw) const
{
    // Same target line <=> equal key. The base's sub-line offset is
    // folded in so unaligned bases stay exact (in hardware the fold
    // happens once at CAM insertion: a 6-bit constant add, never a
    // full-width base add).
    return ((extendRaw(f, raw) << f.shift) + (f.base & (blkSize - 1)))
           >> floorLog2(blkSize);
}

bool
Viper::inDedupWindow(const ConsumerGroup &cg, Addr line) const
{
    for (const auto &prev : cg.dedupWindow) {
        if (std::find(prev.begin(), prev.end(), line) != prev.end()) {
            return true;
        }
    }
    return false;
}

bool
Viper::anyConfigured(const SttEntry &ps) const
{
    for (const auto &cg : ps.groups) {
        if (cg.configured) {
            return true;
        }
    }
    return false;
}

bool
Viper::anyFresh(const SttEntry &ps) const
{
    for (const auto &cg : ps.groups) {
        if (cg.configured && cg.form.gen == tbl->generation()) {
            return true;
        }
    }
    return false;
}

// ---------------------------------------------------------------------
// Stream walk and capture registration (shared with GDP by design)
// ---------------------------------------------------------------------

Addr
Viper::announcedLimit(Addr pc, Addr line_va) const
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

unsigned
Viper::gateBytes(Addr pc, Addr line_va) const
{
    // 0 = no announcement covers the line (or gate off): convert
    // whole-line, the pre-gate behavior.
    const Addr limit = announcedLimit(pc, line_va);
    if (limit == 0) {
        return 0;
    }
    return std::min((Addr)blkSize, limit - line_va);
}

void
Viper::streamAhead(SttEntry &ps, Addr addr, unsigned size,
                         Addr announced_limit,
                         std::vector<AddrPriority> &addresses)
{
    // Never gated on queue room: stream candidates are regenerable and
    // are the engine's fuel — every capture comes from one. Gating them
    // is a positive-feedback collapse, because the queue drains via
    // cache pulls, which starve exactly when misses rise.
    Addr from, to;
    if (prefetchDistance > 0) {
        // VLEN-pinned frontier: the window sits prefetchDistance whole
        // VLEN chunks ahead of the demand chunk's start and is as wide
        // as the demand advance, so partial-vl chunks (short rows, loop
        // tails) keep the full byte lookahead and consecutive windows
        // tile without gaps even when size != vlenBytes (LMUL > 1). A
        // fresh/reset window starts at the frontier by construction, so
        // streamStartAtDistance does not apply here.
        from = addr + (Addr)prefetchDistance * vlenBytes;
        to = from + size;
    } else {
        from = addr + size;
        to = addr + size * (Addr)(streamingDistance + 1);
    }
    Addr line = blockAddress(from);
    if (prefetchDistance == 0 && streamStartAtDistance &&
        ps.limitAddr < line) {
        // Fresh or reset window: jump straight to the frontier. The
        // near-window lines are left to demand (their prefetches would
        // be late-but-coalescing at best), so the frontier gets the
        // issue slots immediately; the skipped chunks' gather targets
        // arrive through the demand-miss capture path instead.
        const Addr frontier =
            blockAddress(addr + size * (Addr)streamingDistance);
        if (frontier > line) {
            line = frontier;
        }
    }
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
        if (line >= ps.limitAddr) {
            addresses.push_back(AddrPriority(line, 0));
            viperStats.streamCandidates++;
        }
    }
    if (line > ps.limitAddr) {
        ps.limitAddr = line;
    }
}

void
Viper::registerCapture(Addr line_pa, Addr line_va, Addr producer_pc,
                             bool secure, unsigned valid_bytes)
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
                                 valid_bytes});
    viperStats.capturesRegistered++;
}

void
Viper::dropRegistration(Addr line_pa, bool secure)
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
Viper::retireRegistration(Addr line_pa, Addr producer_pc,
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
Viper::wasConvertedBy(Addr line_pa, Addr producer_pc,
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
Viper::captureBacklogged() const
{
    // Backlog is the whole in-flight pipeline: queued targets plus
    // targets parked awaiting an MMU translation (indirect targets are
    // nearly all cross-page, so the translation queue is real depth,
    // not a corner case).
    return captureQueueGate != 0 &&
           pfq.size() + pfqMissingTranslation.size() >= captureQueueGate;
}

void
Viper::pfHitInCache()
{
    Base::pfHitInCache(); // keep the stat exactly as before
    // Residency intervals train on every attributed resident-drop:
    // the verdict says "this line is resident", and spatial clustering
    // of such verdicts is what earns suppression confidence.
    if (residencyIntervalRegs != 0 && lastIssuedTargetValid) {
        residencyObserveDrop(lastIssuedTarget);
    }
    lastIssuedTargetValid = false;
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
Viper::captureFromReadHit(SttEntry &ps, const PrefetchInfo &pfi,
                                Addr pc, Addr addr, unsigned size,
                                bool is_secure)
{
    // Same freshness gate the fill path applies: a cleared table leaves
    // stale configured bits behind, and stale ways idle at conversion.
    if (!anyFresh(ps)) {
        viperStats.staleConfigs++;
        return;
    }
    // Same admission backpressure as the registration paths
    // (capture_queue_gate): a congested queue means this batch would
    // sit in the slice buffer until the staleness abort discards it.
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
        if (ps.sliceBuffer.size() >= sliceBufferEntries) {
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
        cap.validBytes = gateBytes(pc, line);
        ps.sliceBuffer.push_back(std::move(cap));
        viperStats.hitsCaptured++;
        retireRegistration(line_pa, pc, is_secure);
    }
}

// ---------------------------------------------------------------------
// Conversion (parallel) and emission (demand-clocked, stall-on-full)
// ---------------------------------------------------------------------

bool
Viper::convertChunk(SttEntry &ps, CapturedLine &cap)
{
    const unsigned width = ps.elemBytes;
    if (width == 0 || width > 8) {
        return true; // line consumed; the latch stays empty
    }
    // limit_gate: slice only the bytes inside the announced extent —
    // the tail of the line straddling the extent end is adjacent
    // NON-index data, and each such raw word would otherwise become
    // a garbage target. A partial trailing element floors away.
    unsigned bytes = cap.data.size();
    if (cap.validBytes != 0 && cap.validBytes < bytes) {
        bytes = cap.validBytes;
    }
    const unsigned elems = bytes / width;
    if (cap.nextElem == 0) {
        // First chunk of this line: charge the beyond-limit tail once
        // and reset the per-line bookkeeping. In vl-window mode the
        // seen-sets survive line starts — they clear only at window
        // boundaries (below) — because a gather chunk spans several
        // index lines.
        const unsigned lineElems = cap.data.size() / width;
        if (elems < lineElems) {
            viperStats.elementsBeyondLimit += lineElems - elems;
        }
        for (auto &cg : ps.groups) {
            if (!vlWindowDedup) {
                cg.lineSeen.clear();
            }
            cg.pendingEmitted.clear();
        }
    }

    const unsigned first = cap.nextElem;

    ConvertedLine batch;
    batch.lineVaddr = cap.lineVaddr;
    batch.targets.reserve((elems - first) * ps.groups.size());

    // One shift-and-add lane group per consumer way, all fed by this
    // one broadcast line: at conversion_lanes=0 the whole line
    // converts at once, which is the point of restricting the chain
    // to an affine form (GDP walks the same payload through its
    // replay pipeline at pipelines_per_gather elements per event
    // instead — the drain compute width is the entire structural
    // difference between the two designs). A nonzero lane budget is
    // ELEMENT slots shared by all ways, charged on any-miss: with
    // pre_lane_dedup every way's CAM compares BEFORE lane
    // allocation, a hit clock-gates that way's lane, and an element
    // every way gates on is compacted out of the budget entirely.
    unsigned slots_used = 0;
    unsigned i = first;
    for (; i < elems && (conversionLanes == 0
                         || slots_used < conversionLanes); i++) {
        if (vlWindowDedup) {
            // vl-aware window: this element's VA crossing into a new
            // vl_window_bytes bucket is (approximately) the next
            // gather instructions' chunk — duplicates beyond it have
            // no intra-instruction coalescing guarantee, so the
            // seen-sets reset (the bucket is an index-VA property,
            // shared by every way).
            const Addr wkey = (cap.lineVaddr + i * width)
                              >> vlWindowShift;
            if (wkey != ps.windowKey) {
                ps.windowKey = wkey;
                for (auto &cg : ps.groups) {
                    cg.lineSeen.clear();
                }
            }
        }
        uint64_t raw = 0;
        std::memcpy(&raw, cap.data.data() + i * width, width);

        bool any_fired = false;
        bool any_gated = false;
        for (unsigned g = 0; g < ps.groups.size(); g++) {
            ConsumerGroup &cg = ps.groups[g];
            // Stale or unconfigured ways idle; fresh ways keep
            // converting (the fully-stale-producer flush lives in
            // drainEmission).
            if (!cg.configured || cg.form.gen != tbl->generation()) {
                continue;
            }
            if (preLaneDedup) {
                const Addr key = preLaneKey(cg.form, letoh(raw));
                if (std::find(cg.lineSeen.begin(), cg.lineSeen.end(),
                              key) != cg.lineSeen.end()) {
                    viperStats.elementsPreDeduped++;
                    any_gated = true;
                    continue; // no lane circuit, no budget share
                }
                any_fired = true;
                viperStats.elementsConverted++;
                const Addr target = applyForm(cg.form, letoh(raw));
                if (target == 0 || (target & 0xffffff0000000000ULL)) {
                    viperStats.targetsFiltered++;
                    continue; // filtered targets never enter the seen-set
                }
                cg.lineSeen.push_back(key);
                batch.targets.push_back({target, (uint8_t)g});
            } else {
                any_fired = true;
                viperStats.elementsConverted++;
                const Addr target = applyForm(cg.form, letoh(raw));
                if (target == 0 || (target & 0xffffff0000000000ULL)) {
                    viperStats.targetsFiltered++;
                    continue;
                }
                // Post-lane compare on target line addresses; the
                // seen-set carries across conversion_lanes chunks so
                // a lane-budget split emits the same distinct lines
                // as whole-line conversion did.
                const Addr line = blockAddress(target);
                if (std::find(cg.lineSeen.begin(), cg.lineSeen.end(),
                              line) != cg.lineSeen.end()) {
                    viperStats.targetsDeduplicated++;
                    continue;
                }
                cg.lineSeen.push_back(line);
                batch.targets.push_back({target, (uint8_t)g});
            }
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
    cap.nextElem = i;
    const bool done = cap.nextElem >= elems;
    if (!done) {
        viperStats.conversionWidthLimited++;
    }

    DPRINTF(Viper, "converted index line VA %#x elems [%u,%u) of %u "
            "x %u ways -> %u targets%s\n",
            cap.lineVaddr, first, i, elems,
            (unsigned)ps.groups.size(),
            (unsigned)batch.targets.size(),
            done ? "" : " [width-limited]");
    if (batch.targets.empty()) {
        return done; // nothing to latch; the caller finalizes if done
    }
    batch.lastChunk = done;
    ps.latch = std::move(batch);
    ps.latchValid = true;
    return done;
}

void
Viper::drainEmission(std::vector<AddrPriority> &addresses)
{
    // Emission budget: free queue slots, but never less than the floor.
    // The floor models the continuous queue drain real hardware has —
    // gem5's queue empties only on cache pulls, which starve under high
    // miss rates, and a zero budget there wedges emission until the
    // staleness abort discards everything. drain_floor=0 gives the pure
    // stall for comparison.
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
        SttEntry &ps = kv.second;
        // The lane array runs once per producer per event: it can
        // drain its output latch and convert AT MOST ONE chunk of
        // the front buffered line into it. Stale-line flushes stay
        // free — they model the broadside invalidate, not the array.
        bool converted = false;
        while (true) {
            if (ps.latchValid) {
                // Form integrity across the capture->emit window (the
                // lazy stand-in for the hardware invalidate pulse).
                // Whole-latch flush only when EVERY way went stale; a
                // single stale way forfeits its own targets below.
                if (!anyFresh(ps)) {
                    viperStats.staleConfigs++;
                    ps.latchValid = false;
                    // The line's dedup entries are forfeited with it.
                    for (auto &cg : ps.groups) {
                        cg.lineSeen.clear();
                        cg.pendingEmitted.clear();
                    }
                    continue;
                }
                // Staleness abort: the walk cursor moved past this
                // index line, so its targets would issue late.
                // Strict <, so demand-miss captures of the CURRENT
                // chunk's own lines survive their capture window.
                if (ps.valid &&
                    ps.latch.lineVaddr < blockAddress(ps.lastAddr)) {
                    viperStats.stalenessAborts++;
                    ps.latchValid = false;
                    for (auto &cg : ps.groups) {
                        cg.lineSeen.clear();
                        cg.pendingEmitted.clear();
                    }
                    continue;
                }
                if (room <= 0 && !dropOnFull) {
                    blocked = true;
                    break; // the stall: finished addresses wait latched
                }
                while (ps.latch.next < ps.latch.targets.size() &&
                       (room > 0 || dropOnFull)) {
                    const ConvertedLine::TargetEntry te =
                        ps.latch.targets[ps.latch.next++];
                    ConsumerGroup &cg = ps.groups[te.group];
                    // A way gone stale mid-latch forfeits its own
                    // targets; the other ways keep emitting.
                    if (!cg.configured ||
                        cg.form.gen != tbl->generation()) {
                        viperStats.staleConfigs++;
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
                    // Residency intervals: a target inside a span the
                    // cache's own drop verdicts have proved resident
                    // is forfeited before it costs a queue slot. The
                    // trim path (observed demand miss) un-learns a
                    // stale span, so a wrong suppression costs one
                    // demand miss, once.
                    if (residencyIntervalRegs != 0 &&
                        residencyCovered(blockAddress(te.addr))) {
                        viperStats.targetsResidencySuppressed++;
                        continue; // no queue slot consumed
                    }
                    if (dedupBufferSize) {
                        cg.pendingEmitted.push_back(
                            blockAddress(te.addr));
                    }
                    if (dropBatchConfidence != 0 ||
                        residencyIntervalRegs != 0) {
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
                    addresses.push_back(AddrPriority(te.addr, 0));
                    viperStats.targetsGenerated++;
                    room--;
                    DPRINTF(Viper, "target: %#x (way %u base %#x "
                            "shift %u)\n", te.addr, te.group,
                            cg.form.base, cg.form.shift);
                }
                if (ps.latch.next < ps.latch.targets.size()) {
                    blocked = true;
                    break; // stalled mid-latch; resume next access
                }
                // A dedup-window entry spans the SOURCE LINE, not one
                // lane-budget chunk: each way's closes with the last
                // chunk. The contribution enters the window even when
                // empty — the window ages by processed lines.
                if (ps.latch.lastChunk) {
                    for (auto &cg : ps.groups) {
                        if (dedupBufferSize) {
                            cg.dedupWindow.push_back(
                                std::move(cg.pendingEmitted));
                            while (cg.dedupWindow.size() >
                                   dedupBufferSize) {
                                cg.dedupWindow.pop_front();
                            }
                        }
                        cg.pendingEmitted.clear();
                    }
                }
                ps.latchValid = false; // fully emitted
                continue;
            }
            // Latch free: convert the next buffered line into it.
            if (ps.sliceBuffer.empty()) {
                break;
            }
            if (converted) {
                line_limited = true;
                break; // the array already ran this event
            }
            CapturedLine &cap = ps.sliceBuffer.front();
            if (!anyFresh(ps)) {
                viperStats.staleConfigs++;
                for (auto &cg : ps.groups) {
                    cg.lineSeen.clear();
                    cg.pendingEmitted.clear();
                }
                ps.sliceBuffer.pop_front();
                continue; // flushed before further compute was paid
            }
            if (ps.valid && cap.lineVaddr < blockAddress(ps.lastAddr)) {
                viperStats.stalenessAborts++;
                for (auto &cg : ps.groups) {
                    cg.lineSeen.clear();
                    cg.pendingEmitted.clear();
                }
                ps.sliceBuffer.pop_front();
                continue; // flushed before further compute was paid
            }
            // The lane groups: up to conversion_lanes element slots
            // per event across all ways; a wider line stays at the
            // buffer front and resumes next event.
            const bool line_done = convertChunk(ps, cap);
            if (line_done) {
                ps.sliceBuffer.pop_front();
                if (!ps.latchValid) {
                    // Line finished without latching anything (all
                    // filtered/deduped): close its dedup entries here.
                    for (auto &cg : ps.groups) {
                        if (dedupBufferSize) {
                            cg.dedupWindow.push_back(
                                std::move(cg.pendingEmitted));
                            while (cg.dedupWindow.size() >
                                   dedupBufferSize) {
                                cg.dedupWindow.pop_front();
                            }
                        }
                        cg.pendingEmitted.clear();
                    }
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
Viper::drainWorkPending() const
{
    for (const auto &kv : streamTrackingTable) {
        const SttEntry &ps = kv.second;
        if (ps.latchValid || !ps.sliceBuffer.empty()) {
            return true;
        }
    }
    return false;
}

void
Viper::scheduleDrain()
{
    if (drainEvent.scheduled() || !drainWorkPending()) {
        return;
    }
    schedule(drainEvent, clockEdge(Cycles(1)));
}

void
Viper::drainTick()
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
    scheduleDrain();
}

// ---------------------------------------------------------------------
// Access, issue and fill hooks
// ---------------------------------------------------------------------

void
Viper::notify(const CacheAccessProbeArg &acc, const PrefetchInfo &pfi)
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
    // Residency intervals: an observed demand MISS inside an interval
    // proves that span is (no longer) resident — trim it. This is the
    // model's only aging mechanism: the cache's own behaviour retires
    // stale intervals, no epoch clock needed. The cost of a stale
    // entry is bounded at one demand miss before the trim.
    if (residencyIntervalRegs != 0 && pfi.isCacheMiss()) {
        residencyObserveMiss(blockAddress(pfi.getAddr()));
    }
    Queued::notify(acc, pfi);
    hitPkt = nullptr;
    scheduleDrain();
}

bool
Viper::residencyCovered(Addr line_va) const
{
    for (const auto &iv : residencyIntervals) {
        if (iv.lo != iv.hi && iv.conf >= residencyConfThreshold &&
            line_va >= iv.lo && line_va < iv.hi) {
            return true;
        }
    }
    return false;
}

void
Viper::residencyObserveDrop(Addr line_va)
{
    viperStats.residencyDropsTrained++;
    const Addr gap = (Addr)residencyGapLines * blkSize;
    // Extend the interval this drop lands in or next to. Residency is
    // contiguous in the target array, so a nearby drop is the same hot
    // span growing (the toy-example leading edge).
    for (auto &iv : residencyIntervals) {
        if (iv.lo == iv.hi) {
            continue;
        }
        if (line_va + blkSize + gap >= iv.lo && line_va < iv.hi + gap) {
            iv.lo = std::min(iv.lo, line_va);
            iv.hi = std::max(iv.hi, line_va + blkSize);
            if (iv.conf < 7) {
                iv.conf++;
            }
            iv.lastUse = ++residencyUseCount;
            // Absorb any interval the extension now overlaps.
            for (auto &other : residencyIntervals) {
                if (&other != &iv && other.lo != other.hi &&
                    other.lo < iv.hi && iv.lo < other.hi) {
                    iv.lo = std::min(iv.lo, other.lo);
                    iv.hi = std::max(iv.hi, other.hi);
                    other.lo = other.hi = 0;
                    other.conf = 0;
                }
            }
            return;
        }
    }
    // No neighbour: allocate over a free register, else the
    // least-confident (oldest on ties). Isolated drops churn here at
    // conf 1 and never reach the suppression threshold — scattered
    // residency leaves the mechanism inert by construction.
    ResidencyInterval *victim = nullptr;
    for (auto &iv : residencyIntervals) {
        if (iv.lo == iv.hi) {
            victim = &iv;
            break;
        }
        if (victim == nullptr || iv.conf < victim->conf ||
            (iv.conf == victim->conf && iv.lastUse < victim->lastUse)) {
            victim = &iv;
        }
    }
    if (victim != nullptr) {
        victim->lo = line_va;
        victim->hi = line_va + blkSize;
        victim->conf = 1;
        victim->lastUse = ++residencyUseCount;
        viperStats.residencyIntervalAllocs++;
    }
}

void
Viper::residencyObserveMiss(Addr line_va)
{
    for (auto &iv : residencyIntervals) {
        if (iv.lo == iv.hi || line_va < iv.lo || line_va >= iv.hi) {
            continue;
        }
        // Keep the larger side of the split; a middle miss in a hot
        // span is rare enough that one register suffices.
        const Addr left = line_va - iv.lo;
        const Addr right = iv.hi - (line_va + blkSize);
        if (left >= right) {
            iv.hi = line_va;
        } else {
            iv.lo = line_va + blkSize;
        }
        if (iv.conf > 0) {
            iv.conf--;
        }
        if (iv.lo >= iv.hi || iv.conf == 0) {
            iv.lo = iv.hi = 0;
            iv.conf = 0;
        }
        viperStats.residencyIntervalTrims++;
        return;
    }
}

void
Viper::calculatePrefetch(const PrefetchInfo &pfi,
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

    const VectorChainTable::ProducerInfo info = tbl->producerInfo(pc);
    if (!info.found) {
        drainEmission(addresses);
        return;
    }

    SttEntry &ps = streamTrackingTable[pc];
    if (addr != ps.lastAddr || !ps.valid) {
        viperStats.chunksObserved++;
        // A jump backwards is a restarted walk: re-prefetch. The dedup
        // windows clear with it — their lines may have been evicted
        // since, and suppressing their re-emission would punch holes
        // in the restarted pass.
        if (ps.valid && addr < ps.lastAddr) {
            ps.limitAddr = 0;
            for (auto &cg : ps.groups) {
                cg.dedupWindow.clear();
            }
        }
        ps.lastAddr = addr;
        ps.valid = true;

        // Architectural stream (no confidence: the opcode said
        // unit-stride).
        streamAhead(ps, addr, size, announcedLimit(pc, addr), addresses);

        // Adopt this producer's linear forms, one per linked consumer
        // slot. The table walks each chain on demand (consumer->head,
        // bounded); collapse() folds it into (base, shift) here, so
        // the runtime never sees the stages. Operands read here are
        // the freshest snooped values; ways configure independently
        // (one base snoopable, another not yet is fine).
        if (!streamOnly && info.linked) {
            if (!anyConfigured(ps) && configuredCount >= pipelines) {
                viperStats.pipelinesSaturated++;
            } else {
                const unsigned nways =
                    std::min(info.numConsumers, consumersPerProducer);
                if (ps.groups.size() < nways) {
                    ps.groups.resize(nways);
                }
                for (unsigned s = 0; s < nways; s++) {
                    // Memo: the compiled form is a pure function of
                    // the chain's shape (generation) and its snooped
                    // operands (operandGen). While both still match,
                    // re-walking and re-folding would reproduce the
                    // same (base, shift, ext) bit for bit — so the
                    // chain is compiled ONCE per real change, not once
                    // per observed chunk.
                    if (ps.groups[s].configured &&
                        ps.groups[s].form.valid &&
                        ps.groups[s].form.gen == tbl->generation() &&
                        ps.groups[s].form.opGen == tbl->operandGen()) {
                        viperStats.formsMemoHits++;
                        continue;
                    }
                    const VectorChainTable::ChainSnapshot snap =
                        tbl->pipelineConfig(info.dctPtr, s);
                    if (!snap.valid) {
                        viperStats.chainNotReady++;
                        continue;
                    }
                    const LinearForm f = collapse(snap);
                    if (!f.valid) {
                        // Not an A[B[i]] shape. Reject rather than
                        // approximate: this is exactly the class of
                        // pattern GDP's replay pipeline exists for.
                        viperStats.chainsRejected++;
                        continue;
                    }
                    if (!anyConfigured(ps)) {
                        configuredCount++;
                    }
                    ps.groups[s].form = f;
                    ps.groups[s].configured = true;
                    ps.elemBytes = info.elemBytes;
                    viperStats.formsAdopted++;
                    DPRINTF(Viper, "PC %#x way %u form adopted: "
                            "base=%#x shift=%u ext=%u/%s eew=%uB "
                            "(%u lanes)\n",
                            pc, s, f.base, f.shift, f.extBits,
                            f.extSigned ? "s" : "u", info.elemBytes,
                            info.elemBytes ? blkSize / info.elemBytes
                                           : 0);
                }
            }
        }
    }

    // Read-hit capture path (capture_on_read_hit). An index line that is
    // already resident produces no fill, so the registration taken for it
    // can never latch — and on a multi-chain loop that is the common case,
    // because each producer's stream walk covers its siblings' slices and
    // makes their index lines resident before their own demands arrive.
    // The read that hits carries the payload, so take the capture here
    // instead of waiting for a fill that will never come.
    if (captureOnReadHit && hitPkt != nullptr && !streamOnly &&
        anyConfigured(ps) && !pfi.isCacheMiss()) {
        captureFromReadHit(ps, pfi, pc, addr, size, is_secure);
    }

    // Emit converted targets after the stream's own candidates.
    drainEmission(addresses);
}

void
Viper::rowSchedule()
{
    if (rowScheduleBits == 0 || !lastRowValid || pfq.size() < 2) {
        return;
    }
    // Only reorder when the head is already due: every candidate we
    // consider could have issued this cycle, so promoting one delays
    // nothing past its ready time and leaves nextPrefetchReadyTime
    // (pfq.front().tick) reporting a tick that is still due.
    const DeferredPacket &head = pfq.front();
    if (head.pkt == nullptr || head.tick > curTick()) {
        return;
    }
    // The queue is sorted by priority (addToQueue, queued.cc), so the
    // head's priority group is the leading run; stop at its end rather
    // than hoisting a lower-priority packet over a higher-priority one.
    // The scan is further bounded to the first reorderWindowSize queue
    // entries, head included (0 = whole queue): a fixed-depth
    // comparator window, not a CAM over every entry.
    unsigned pos = 1;
    for (auto it = std::next(pfq.begin()); it != pfq.end(); ++it, ++pos) {
        if (reorderWindowSize != 0 && pos >= reorderWindowSize) {
            break;
        }
        if (it->priority != head.priority) {
            break;
        }
        if (it->pkt == nullptr || it->tick > curTick()) {
            continue;
        }
        if ((it->pkt->getAddr() >> rowScheduleBits) != lastRow) {
            continue;
        }
        pfq.splice(pfq.begin(), pfq, it);
        viperStats.rowPromotions++;
        return;
    }
    viperStats.rowScheduleMisses++;
}

PacketPtr
Viper::getPacket()
{
    // Reorder before the capture peek below: that peek must see the
    // entry Queued::getPacket will actually pop.
    rowSchedule();
    // Range-based capture: a prefetch issuing inside a configured
    // producer's stream window is index data — learn the PA that
    // notifyFill will see. The issued packet's request is PA-only
    // (createPkt, queued.cc), so peek the DeferredPacket BEFORE
    // delegating: its PrefetchInfo still holds the VA the candidate was
    // generated with, and its pkt holds the translated PA.
    lastIssuedValid = false;
    lastIssuedTargetValid = false;
    if (!pfq.empty() && pfq.front().pkt != nullptr) {
        const DeferredPacket &dp = pfq.front();
        const Addr line_va = blockAddress(dp.pfInfo.getAddr());
        // drop_batch_confidence: consume this target's batch tag. The
        // cache's residency check runs synchronously after this call
        // returns, so if the packet is dropped, pfHitInCache() below
        // can attribute the drop to this batch. Stream candidates
        // carry no tag and stay invisible to the feedback.
        if (dropBatchConfidence != 0 || residencyIntervalRegs != 0) {
            auto tag = emittedFrom.find(line_va);
            if (tag != emittedFrom.end()) {
                lastIssuedValid = true;
                lastIssuedSource = tag->second;
                // Residency intervals need the TARGET line of the
                // packet being handed over; the tag's presence is what
                // marks it as an indirect target (streams carry none).
                lastIssuedTargetValid = true;
                lastIssuedTarget = line_va;
                emittedFrom.erase(tag);
            }
        }
        const Addr line_pa = blockAddress(dp.pkt->getAddr());
        const bool secure = dp.pfInfo.isSecure();
        for (auto &kv : streamTrackingTable) {
            SttEntry &ps = kv.second;
            if (!ps.valid) {
                continue;
            }
            const Addr win_lo = blockAddress(ps.lastAddr) + blkSize;
            if (line_va < win_lo || line_va >= ps.limitAddr) {
                continue;
            }
            // This departing prefetch is a stream candidate of some
            // producer (configured or not) — this is the only point
            // where a stream line's VA and PA are held together, so
            // publish its physical page for stream-aware replacement.
            tbl->registerStreamPage(line_pa);
            // Admission backpressure (capture_queue_gate): with the
            // queue saturated, emission is stalled and the demand
            // cursor will pass this line before its batch can issue —
            // the capture would only feed the staleness abort. The
            // stream prefetch itself still departs; only the
            // capture/convert half sheds load.
            if (anyConfigured(ps)) {
                if (captureBacklogged()) {
                    viperStats.capturesGated++;
                } else {
                    registerCapture(line_pa, line_va, kv.first, secure,
                                    gateBytes(kv.first, line_va));
                }
            }
            break;
        }
    }
    PacketPtr pkt = Queued::getPacket();
    if (rowScheduleBits != 0 && pkt != nullptr) {
        lastRow = pkt->getAddr() >> rowScheduleBits;
        lastRowValid = true;
    }
    // A pull frees queue room — the room-gated drain can run again.
    scheduleDrain();
    return pkt;
}

void
Viper::notifyFill(const CacheAccessProbeArg &acc)
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
    // dropped on a stale form or a full slice buffer converted nothing,
    // so a later read hit on this line is still real work.
    indexRoutingTable.erase(it);

    auto ps_it = streamTrackingTable.find(rec.producerPC);
    if (ps_it == streamTrackingTable.end() ||
        !anyConfigured(ps_it->second)) {
        return;
    }
    SttEntry &ps = ps_it->second;
    // Lazy stand-in for the hardware invalidate pulse: a wholesale table
    // clear would flash-clear the configured bits; the sim compares
    // generations at the next use instead. Any fresh way justifies
    // the capture — stale ways idle at conversion.
    if (!anyFresh(ps)) {
        viperStats.staleConfigs++;
        return;
    }
    if (pkt->getSize() < blkSize) {
        return;
    }

    // Chunk-granular load shedding: a fill arriving with the slice
    // buffer full is dropped whole, because one uncovered line stalls
    // the gather anyway.
    if (ps.sliceBuffer.size() >= sliceBufferEntries) {
        viperStats.bufferBusyDrops++;
        return;
    }

    // Latch the raw payload, exactly as GDP does; conversion happens
    // at the drain, one line per event through the lane array.
    CapturedLine cap;
    const uint8_t *data = pkt->getConstPtr<uint8_t>();
    cap.data.assign(data, data + pkt->getSize());
    cap.lineVaddr = blockAddress(rec.lineVaddr);
    cap.validBytes = rec.validBytes;
    ps.sliceBuffer.push_back(std::move(cap));
    viperStats.fillsCaptured++;
    retireRegistration(line_pa, rec.producerPC, pkt->isSecure());
    DPRINTF(Viper, "fill captured: index line VA %#x (producer %#x)\n",
            rec.lineVaddr, rec.producerPC);
    // Fresh conversion work: arm the self-clocked drain.
    scheduleDrain();
}

} // namespace prefetch
} // namespace gem5
