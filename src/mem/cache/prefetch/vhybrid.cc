/**
 * VHybrid implementation. See vhybrid.hh for the composition; the
 * stream half follows revela.cc and the indirect half vector_tyche.cc,
 * each structurally unchanged so results decompose against the
 * standalone parents.
 */

#include "mem/cache/prefetch/vhybrid.hh"

#include <algorithm>
#include <cstring>

#include "base/bitfield.hh"
#include "base/intmath.hh"
#include "base/logging.hh"
#include "base/trace.hh"
#include "debug/VHybrid.hh"
#include "params/VHybridPrefetcher.hh"
#include "sim/byteswap.hh"

namespace gem5
{

namespace prefetch
{

VHybrid::VHybrid(const VHybridPrefetcherParams &p)
  : Queued(p),
    streamTbl(p.stream_table),
    chainTbl(p.link_table),
    maxPrefetchDistance(p.max_prefetch_distance),
    degree(p.degree),
    initialDistance(p.initial_distance),
    streamOnly(p.stream_only),
    sliceBufferEntries(p.slice_buffer_entries),
    pipelines(p.pipelines),
    irtEntries(p.routing_entries),
    drainFloor(p.drain_floor),
    dedupBufferSize(p.dedup_buffer_size),
    limitAwareSlicing(p.limit_aware_slicing),
    conversionLanes(p.conversion_lanes),
    dropOnFull(p.drop_on_full),
    vlWindowDedup(p.vl_window_dedup),
    vlWindowBytes(p.vl_window_bytes),
    vlWindowShift(isPowerOf2(p.vl_window_bytes)
                  ? floorLog2(p.vl_window_bytes) : 0),
    preLaneDedup(p.pre_lane_dedup),
    pipeTable(),
    indexRoutingTable(),
    vhybridStats(this),
    drainEvent([this] { drainTick(); }, name())
{
    fatal_if(streamTbl == nullptr, "%s: no stream_table set. VHybrid "
             "needs the RevelaStreamTable that is also attached to the "
             "CPU's revela_table param (the config script wires both).",
             name());
    fatal_if(chainTbl == nullptr, "%s: no link_table set. VHybrid "
             "needs the VectorChainTable that is also attached to the "
             "CPU's vector_chain_table param (the config script wires "
             "both).", name());
    fatal_if(maxPrefetchDistance < 1, "max_prefetch_distance must be "
             ">= 1");
    fatal_if(degree < 1, "degree must be >= 1");
    fatal_if(sliceBufferEntries < 1, "slice_buffer_entries must be >= 1");
    fatal_if(pipelines < 1, "pipelines must be >= 1");
    fatal_if(drainFloor < 0, "drain_floor must be >= 0");
    fatal_if(vlWindowDedup && !isPowerOf2(vlWindowBytes),
             "vl_window_bytes must be a power of two");
    fatal_if(vlWindowDedup && vlWindowBytes < blkSize,
             "vl_window_bytes must be >= the cache line size");
}

void
VHybrid::resetLearnedState()
{
    // Both sideband tables register their own reset callbacks; only
    // this prefetcher's runtime state is cleared here.
    pipeTable.clear();
    configuredCount = 0;
    indexRoutingTable.clear();
}

VHybrid::VHybridStats::VHybridStats(statistics::Group *parent)
  : statistics::Group(parent),
    ADD_STAT(selfDrainTicks, statistics::units::Count::get(),
        "self-clocked drain firings that evaluated the trigger"),
    ADD_STAT(linesEmitted, statistics::units::Count::get(),
        "stream lines emitted into the queue"),
    ADD_STAT(drainNoRoom, statistics::units::Count::get(),
        "stream rounds skipped for lack of queue room"),
    ADD_STAT(aggressivityThrottled, statistics::units::Count::get(),
        "stream-emissions skipped by the aggressivity cap"),
    ADD_STAT(linesSkippedFloor, statistics::units::Count::get(),
        "stream lines never emitted: the initial-distance floor "
        "jumped the frontier past them"),
    ADD_STAT(producerChunks, statistics::units::Count::get(),
        "chunk events at producer PCs"),
    ADD_STAT(formsAdopted, statistics::units::Count::get(),
        "linear forms adopted at trigger"),
    ADD_STAT(chainNotReady, statistics::units::Count::get(),
        "linked triggers whose chain was not yet snoopable"),
    ADD_STAT(chainsRejected, statistics::units::Count::get(),
        "chains rejected: not collapsible to base + (idx << shift)"),
    ADD_STAT(pipelinesSaturated, statistics::units::Count::get(),
        "linked producers denied a pipeline (pipelines cap)"),
    ADD_STAT(capturesRegistered, statistics::units::Count::get(),
        "index lines registered for capture (prefetch-window path)"),
    ADD_STAT(capturesRegisteredMiss, statistics::units::Count::get(),
        "index lines registered for capture (demand-miss path)"),
    ADD_STAT(fillsCaptured, statistics::units::Count::get(),
        "captured index-line fills latched into the slice buffer"),
    ADD_STAT(bufferBusyDrops, statistics::units::Count::get(),
        "fills dropped with the slice buffer full"),
    ADD_STAT(staleConfigs, statistics::units::Count::get(),
        "fills dropped on a stale form (table cleared)"),
    ADD_STAT(elementsConverted, statistics::units::Count::get(),
        "elements pushed through a shift-and-add lane"),
    ADD_STAT(elementsBeyondLimit, statistics::units::Count::get(),
        "line-tail elements NOT converted: beyond the announced "
        "stream extent (not index data)"),
    ADD_STAT(conversionWidthLimited, statistics::units::Count::get(),
        "conversion events cut short by the lane budget (the line "
        "resumed on a later event)"),
    ADD_STAT(elementsPreDeduped, statistics::units::Count::get(),
        "elements dropped by the pre-lane truncated-index compare "
        "(consumed no shift/add lane and no lane-budget slot)"),
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
        "batches flushed: stream cursor passed their index line"),
    ADD_STAT(emissionDeferred, statistics::units::Count::get(),
        "emission events paused by a full queue (the stall firing)"),
    ADD_STAT(emissionLineLimited, statistics::units::Count::get(),
        "events where the array's one-line conversion slot ended "
        "with further lines still buffered")
{
}

// ---------------------------------------------------------------------
// Stream half (ReVeLA logic against the shared STT)
// ---------------------------------------------------------------------

unsigned
VHybrid::aggressivityLimit() const
{
    const unsigned valid = streamTbl->numValid();
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
    // that would throttle it forever (see revela.cc).
    return std::max(std::max(limit, initialDistance), 1U);
}

unsigned
VHybrid::distanceLines(const RevelaStreamTable::SttEntry &e) const
{
    if (e.prefetchedUpTo <= e.currentAddr) {
        return 0;
    }
    return (e.prefetchedUpTo - e.currentAddr + blkSize - 1) / blkSize;
}

void
VHybrid::applyInitialDistance(RevelaStreamTable::SttEntry &e)
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
    vhybridStats.linesSkippedFloor +=
        (blockAddress(jump_to) - blockAddress(e.prefetchedUpTo))
        / blkSize;
    e.prefetchedUpTo = jump_to;
}

bool
VHybrid::streamWorkPending() const
{
    const unsigned limit = aggressivityLimit();
    for (const auto &e : streamTbl->entries()) {
        if (e.valid && e.prefetchedUpTo < e.limitAddr &&
            distanceLines(e) <= limit) {
            return true;
        }
    }
    return false;
}

void
VHybrid::emitStreamRound(std::vector<AddrPriority> &addresses)
{
    // One combined budget with the targets staged earlier this event.
    int room = (int)queueSize - (int)pfq.size()
             - (int)pfqMissingTranslation.size()
             - (int)addresses.size();
    if (room <= 0) {
        vhybridStats.drainNoRoom++;
        return;
    }

    const unsigned limit = aggressivityLimit();

    unsigned min_dist = 0;
    bool have_min = false;
    for (auto &e : streamTbl->entries()) {
        if (!e.valid || e.prefetchedUpTo >= e.limitAddr) {
            continue;
        }
        applyInitialDistance(e);
        if (e.prefetchedUpTo >= e.limitAddr) {
            continue; // floored past the extent end
        }
        const unsigned d = distanceLines(e);
        if (d > limit) {
            vhybridStats.aggressivityThrottled++;
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

    for (auto &e : streamTbl->entries()) {
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
            vhybridStats.linesEmitted++;
        }
    }
}

// ---------------------------------------------------------------------
// The collapse and the lane (VTyche's, unchanged)
// ---------------------------------------------------------------------

VHybrid::LinearForm
VHybrid::collapse(const VectorChainTable::ChainSnapshot &snap) const
{
    using VOp = VectorChainTable::VOp;
    LinearForm f;
    f.gen = snap.gen;
    if (!snap.valid) {
        return f;
    }

    unsigned shift = 0;
    uint64_t bias = 0;
    bool scaled = false;
    bool extended = false;

    for (const auto &s : snap.ops) {
        switch (s.op) {
          case VOp::SExt:
          case VOp::ZExt:
            if (scaled || extended) {
                return LinearForm();
            }
            extended = true;
            f.extBits = s.extFromBits;
            f.extSigned = (s.op == VOp::SExt);
            break;

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

          default:
            return LinearForm();
        }
    }

    f.base = snap.base + bias;
    f.shift = shift;
    f.valid = true;
    return f;
}

uint64_t
VHybrid::extendRaw(const LinearForm &f, uint64_t raw) const
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
VHybrid::applyForm(const LinearForm &f, uint64_t raw) const
{
    return f.base + (extendRaw(f, raw) << f.shift);
}

Addr
VHybrid::preLaneKey(const LinearForm &f, uint64_t raw) const
{
    // Same target line <=> equal key. The base's sub-line offset is
    // folded in so unaligned bases stay exact (in hardware the fold
    // happens once at CAM insertion: a 6-bit constant add, never a
    // full-width base add).
    return ((extendRaw(f, raw) << f.shift) + (f.base & (blkSize - 1)))
           >> floorLog2(blkSize);
}

bool
VHybrid::inDedupWindow(const PipeEntry &pe, Addr line) const
{
    for (const auto &prev : pe.dedupWindow) {
        if (std::find(prev.begin(), prev.end(), line) != prev.end()) {
            return true;
        }
    }
    return false;
}

// ---------------------------------------------------------------------
// Capture, conversion and target emission (VTyche's runtime; the
// staleness cursor comes from the announced stream instead of a walk)
// ---------------------------------------------------------------------

void
VHybrid::registerCapture(Addr line_pa, Addr line_va, Addr producer_pc,
                         bool secure, bool from_miss,
                         unsigned valid_bytes)
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
    if (from_miss) {
        vhybridStats.capturesRegisteredMiss++;
    } else {
        vhybridStats.capturesRegistered++;
    }
}

unsigned
VHybrid::clampBytes(Addr limit_addr, Addr line_va) const
{
    // 0 = no announcement covers the line; convert whole-line as
    // before. A registered line always starts below its limit (the
    // stream never emits at/past limit@), so the subtraction is safe.
    if (!limitAwareSlicing || limit_addr == 0 || limit_addr <= line_va) {
        return blkSize;
    }
    return std::min((Addr)blkSize, limit_addr - line_va);
}

Addr
VHybrid::announcedLimit(Addr pc, Addr line_va) const
{
    // Same aliasing rule as streamCursor: one PC can have several
    // live entries (e.g. one per jagged diagonal). Take the SMALLEST
    // limit still above the line — the most conservative clamp; a
    // mis-attribution can only suppress tail conversions (a lost
    // prefetch opportunity), never fabricate extra ones.
    Addr limit = 0;
    for (const auto &e : streamTbl->entries()) {
        if (e.valid && e.lastPc == pc && e.limitAddr > line_va &&
            (limit == 0 || e.limitAddr < limit)) {
            limit = e.limitAddr;
        }
    }
    return limit;
}

Addr
VHybrid::streamCursor(Addr pc) const
{
    // The producer's live stream is the STT entry its accesses last
    // updated. The same PC allocates a fresh entry per announced
    // extent (e.g. one per jagged diagonal), and entries can overlap
    // briefly in flight — take the minimum cursor so the staleness
    // abort never outruns the least-advanced live stream.
    Addr cursor = 0;
    for (const auto &e : streamTbl->entries()) {
        if (e.valid && e.lastPc == pc &&
            (cursor == 0 || e.currentAddr < cursor)) {
            cursor = e.currentAddr;
        }
    }
    return cursor;
}

bool
VHybrid::convertChunk(PipeEntry &pe, CapturedLine &cap)
{
    const unsigned width = pe.elemBytes;
    if (width == 0 || width > 8) {
        return true; // line consumed; the latch stays empty
    }
    // Slice only the bytes inside the announced extent: the tail of
    // the line straddling limit@ is adjacent NON-index data, and each
    // such raw word would otherwise become a garbage target. A
    // partial trailing element (limit not width-aligned) floors away.
    unsigned bytes = cap.data.size();
    if (cap.validBytes != 0 && cap.validBytes < bytes) {
        bytes = cap.validBytes;
    }
    const unsigned elems = bytes / width;
    if (cap.nextElem == 0) {
        // First chunk of this line: charge the beyond-limit tail once
        // and reset the per-line bookkeeping. In vl-window mode the
        // seen-set survives line starts — it clears only at window
        // boundaries (below) — because a gather chunk spans several
        // index lines.
        const unsigned lineElems = cap.data.size() / width;
        if (elems < lineElems) {
            vhybridStats.elementsBeyondLimit += lineElems - elems;
        }
        if (!vlWindowDedup) {
            pe.lineSeen.clear();
        }
        pe.pendingEmitted.clear();
    }

    const unsigned first = cap.nextElem;

    ConvertedLine batch;
    batch.lineVaddr = cap.lineVaddr;
    batch.targets.reserve(elems - first);

    // The lane budget: how many shift-and-add circuits fire this
    // event (0 = unbounded). With pre_lane_dedup, a window hit is
    // filtered BEFORE lane allocation and does not spend a slot.
    unsigned lanes_used = 0;
    unsigned i = first;
    for (; i < elems && (conversionLanes == 0
                         || lanes_used < conversionLanes); i++) {
        if (vlWindowDedup) {
            // vl-aware window: this element's VA crossing into a new
            // vl_window_bytes bucket is (approximately) the next
            // gather instruction's chunk — duplicates beyond it have
            // no intra-instruction coalescing guarantee, so the
            // seen-set resets.
            const Addr wkey = (cap.lineVaddr + i * width)
                              >> vlWindowShift;
            if (wkey != pe.windowKey) {
                pe.windowKey = wkey;
                pe.lineSeen.clear();
            }
        }
        uint64_t raw = 0;
        std::memcpy(&raw, cap.data.data() + i * width, width);

        if (preLaneDedup) {
            const Addr key = preLaneKey(pe.form, letoh(raw));
            if (std::find(pe.lineSeen.begin(), pe.lineSeen.end(), key)
                != pe.lineSeen.end()) {
                vhybridStats.elementsPreDeduped++;
                continue; // no lane circuit, no budget slot
            }
            lanes_used++;
            vhybridStats.elementsConverted++;
            const Addr target = applyForm(pe.form, letoh(raw));
            if (target == 0 || (target & 0xffffff0000000000ULL)) {
                vhybridStats.targetsFiltered++;
                continue; // filtered targets never enter the seen-set
            }
            pe.lineSeen.push_back(key);
            batch.targets.push_back(target);
        } else {
            lanes_used++;
            vhybridStats.elementsConverted++;
            const Addr target = applyForm(pe.form, letoh(raw));
            if (target == 0 || (target & 0xffffff0000000000ULL)) {
                vhybridStats.targetsFiltered++;
                continue;
            }
            // Post-lane compare on target line addresses; the
            // seen-set carries across conversion_lanes chunks so a
            // lane-budget split emits the same distinct lines as
            // whole-line conversion did.
            const Addr line = blockAddress(target);
            if (std::find(pe.lineSeen.begin(), pe.lineSeen.end(),
                          line) != pe.lineSeen.end()) {
                vhybridStats.targetsDeduplicated++;
                continue;
            }
            pe.lineSeen.push_back(line);
            batch.targets.push_back(target);
        }
    }
    cap.nextElem = i;
    const bool done = cap.nextElem >= elems;
    if (!done) {
        vhybridStats.conversionWidthLimited++;
    }

    DPRINTF(VHybrid, "converted index line VA %#x elems [%u,%u) of %u "
            "-> %u distinct target lines (base %#x shift %u)%s\n",
            cap.lineVaddr, first, i, elems,
            (unsigned)batch.targets.size(), pe.form.base,
            pe.form.shift, done ? "" : " [width-limited]");
    if (batch.targets.empty()) {
        return done; // nothing to latch; the caller finalizes if done
    }
    batch.lastChunk = done;
    pe.latch = std::move(batch);
    pe.latchValid = true;
    return done;
}

void
VHybrid::drainEmission(std::vector<AddrPriority> &addresses)
{
    int room = (int)queueSize - (int)pfq.size()
             - (int)pfqMissingTranslation.size()
             - (int)addresses.size();
    // drop_on_full replaces the drain_floor displacement license:
    // room is never forced, and overflow targets are discarded
    // below instead of pushed into a full queue (where
    // Queued::insert would evict staged entries).
    if (room < drainFloor && !dropOnFull) {
        room = drainFloor;
    }
    bool blocked = false;
    bool line_limited = false;
    for (auto &kv : pipeTable) {
        if (blocked) {
            break;
        }
        PipeEntry &pe = kv.second;
        const Addr cursor = streamCursor(kv.first);
        bool converted = false;
        while (true) {
            if (pe.latchValid) {
                if (!pe.configured ||
                    pe.form.gen != chainTbl->generation()) {
                    vhybridStats.staleConfigs++;
                    pe.latchValid = false;
                    // The line's dedup entry is forfeited with it.
                    pe.lineSeen.clear();
                    pe.pendingEmitted.clear();
                    continue;
                }
                // Staleness abort against the announced stream's
                // demand cursor (strict <: the current chunk's own
                // lines survive their capture window).
                if (cursor != 0 &&
                    pe.latch.lineVaddr < blockAddress(cursor)) {
                    vhybridStats.stalenessAborts++;
                    pe.latchValid = false;
                    pe.lineSeen.clear();
                    pe.pendingEmitted.clear();
                    continue;
                }
                if (room <= 0 && !dropOnFull) {
                    blocked = true;
                    break; // the stall: finished addresses wait latched
                }
                while (pe.latch.next < pe.latch.targets.size() &&
                       (room > 0 || dropOnFull)) {
                    const Addr target =
                        pe.latch.targets[pe.latch.next++];
                    if (dedupBufferSize) {
                        const Addr line = blockAddress(target);
                        if (inDedupWindow(pe, line)) {
                            vhybridStats.targetsCrossDeduplicated++;
                            continue; // no queue slot consumed
                        }
                    }
                    if (room <= 0) {
                        // drop_on_full: the queue keeps its staged
                        // entries; the overflow target is forfeited.
                        // NOT recorded emitted — no prefetch went
                        // out, so a later duplicate may re-emit.
                        vhybridStats.targetsDroppedFull++;
                        continue;
                    }
                    if (dedupBufferSize) {
                        pe.pendingEmitted.push_back(blockAddress(target));
                    }
                    addresses.push_back(AddrPriority(target, 0));
                    vhybridStats.targetsGenerated++;
                    room--;
                    DPRINTF(VHybrid, "target: %#x (base %#x shift "
                            "%u)\n", target, pe.form.base,
                            pe.form.shift);
                }
                if (pe.latch.next < pe.latch.targets.size()) {
                    blocked = true;
                    break; // stalled mid-latch; resume next event
                }
                // A dedup-window entry spans the SOURCE LINE, not one
                // lane-budget chunk: it closes with the last chunk.
                if (pe.latch.lastChunk) {
                    if (dedupBufferSize) {
                        pe.dedupWindow.push_back(
                            std::move(pe.pendingEmitted));
                        while (pe.dedupWindow.size() > dedupBufferSize) {
                            pe.dedupWindow.pop_front();
                        }
                    }
                    pe.pendingEmitted.clear();
                }
                pe.latchValid = false; // fully emitted
                continue;
            }
            if (pe.sliceBuffer.empty()) {
                break;
            }
            if (converted) {
                line_limited = true;
                break; // the array already ran this event
            }
            CapturedLine &cap = pe.sliceBuffer.front();
            if (!pe.configured || pe.form.gen != chainTbl->generation()) {
                vhybridStats.staleConfigs++;
                pe.lineSeen.clear();
                pe.pendingEmitted.clear();
                pe.sliceBuffer.pop_front();
                continue; // flushed before further compute was paid
            }
            if (cursor != 0 && cap.lineVaddr < blockAddress(cursor)) {
                vhybridStats.stalenessAborts++;
                pe.lineSeen.clear();
                pe.pendingEmitted.clear();
                pe.sliceBuffer.pop_front();
                continue; // flushed before further compute was paid
            }
            // The lane array: up to conversion_lanes elements per
            // event; a wider line stays at the buffer front and
            // resumes next event.
            const bool line_done = convertChunk(pe, cap);
            if (line_done) {
                pe.sliceBuffer.pop_front();
                if (!pe.latchValid) {
                    // Line finished without latching anything (all
                    // filtered/deduped): close its dedup entry here.
                    if (dedupBufferSize) {
                        pe.dedupWindow.push_back(
                            std::move(pe.pendingEmitted));
                        while (pe.dedupWindow.size() > dedupBufferSize) {
                            pe.dedupWindow.pop_front();
                        }
                    }
                    pe.pendingEmitted.clear();
                }
            }
            converted = true;
        }
    }
    if (blocked) {
        vhybridStats.emissionDeferred++;
    }
    if (line_limited) {
        vhybridStats.emissionLineLimited++;
    }
}

bool
VHybrid::conversionWorkPending() const
{
    for (const auto &kv : pipeTable) {
        const PipeEntry &pe = kv.second;
        if (pe.latchValid || !pe.sliceBuffer.empty()) {
            return true;
        }
    }
    return false;
}

// ---------------------------------------------------------------------
// Self-clocked drain (one event owns both halves)
// ---------------------------------------------------------------------

void
VHybrid::scheduleDrain()
{
    if (drainEvent.scheduled() ||
        (!streamWorkPending() && !conversionWorkPending())) {
        return;
    }
    // Every cycle, ReVeLA's fixed trigger cadence.
    schedule(drainEvent, clockEdge(Cycles(1)));
}

void
VHybrid::drainTick()
{
    // No context until the first demand access has been observed; the
    // next notify() re-arms.
    if (!drainCtxPfi || !drainCtxCache) {
        return;
    }
    // Wait for genuine queue room (see vector_tyche.cc drainTick):
    // room only appears through getPacket or notify, and both re-arm.
    const int room = (int)queueSize - (int)pfq.size()
                   - (int)pfqMissingTranslation.size();
    if (room < std::max(drainFloor, 1)) {
        return;
    }
    vhybridStats.selfDrainTicks++;
    std::vector<AddrPriority> addresses;
    // Targets first — their gather demand is closer than any stream
    // frontier, and stream candidates regenerate next tick for free.
    drainEmission(addresses);
    emitStreamRound(addresses);
    if (!addresses.empty()) {
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
    // Kick pending cross-page translations (revela.cc: upstream only
    // starts them from getPacket, which the cache stops calling once
    // pfq is empty — without this the drain wedges).
    if (!pfqMissingTranslation.empty()) {
        processMissingTranslations(queueSize - pfq.size());
    }
    scheduleDrain();
}

// ---------------------------------------------------------------------
// Access, issue and fill hooks
// ---------------------------------------------------------------------

void
VHybrid::notify(const CacheAccessProbeArg &acc, const PrefetchInfo &pfi)
{
    if (acc.pkt != nullptr && acc.pkt->req != nullptr &&
        acc.pkt->req->hasVaddr() && acc.pkt->req->hasContextId()) {
        drainCtxReq = acc.pkt->req;
        drainCtxPfi.reset(new PrefetchInfo(pfi, pfi.getAddr()));
        drainCtxCache = &acc.cache;
    }
    Queued::notify(acc, pfi);
    if (!pfqMissingTranslation.empty()) {
        processMissingTranslations(queueSize - pfq.size());
    }
    scheduleDrain();
}

void
VHybrid::calculatePrefetch(const PrefetchInfo &pfi,
                           std::vector<AddrPriority> &addresses,
                           const CacheAccessor &cache)
{
    // Producer adoption + demand-miss capture need a read with a PC;
    // emission runs on every observed access regardless (the event
    // carries the translation context it needs).
    if (!pfi.isWrite() && pfi.hasPC() && !streamOnly) {
        const Addr addr = pfi.getAddr();
        const unsigned size = pfi.getSize();
        const Addr pc = pfi.getPC();
        const bool is_secure = pfi.isSecure();

        const VectorChainTable::ProducerInfo info =
            chainTbl->producerInfo(pc);
        if (info.found) {
            PipeEntry &pe = pipeTable[pc];
            if (addr != pe.lastAddr || !pe.valid) {
                vhybridStats.producerChunks++;
                pe.lastAddr = addr;
                pe.valid = true;
                // Adopt/refresh this producer's linear form with the
                // freshest snooped operands (vector_tyche.cc).
                if (info.linked) {
                    if (!pe.configured && configuredCount >= pipelines) {
                        vhybridStats.pipelinesSaturated++;
                    } else {
                        const VectorChainTable::ChainSnapshot snap =
                            chainTbl->pipelineConfig(info.dctPtr);
                        if (!snap.valid) {
                            vhybridStats.chainNotReady++;
                        } else {
                            const LinearForm f = collapse(snap);
                            if (!f.valid) {
                                vhybridStats.chainsRejected++;
                            } else {
                                if (!pe.configured) {
                                    configuredCount++;
                                }
                                pe.form = f;
                                pe.configured = true;
                                pe.elemBytes = info.elemBytes;
                                vhybridStats.formsAdopted++;
                                DPRINTF(VHybrid, "PC %#x form adopted: "
                                        "base=%#x shift=%u ext=%u/%s "
                                        "eew=%uB\n", pc, f.base, f.shift,
                                        f.extBits,
                                        f.extSigned ? "s" : "u",
                                        info.elemBytes);
                            }
                        }
                    }
                }
            }
            // Demand-miss capture: the index array's own misses carry
            // both VA and PA — register their lines directly. No STT
            // entry is in hand here; the announced extent is resolved
            // by PC (announcedLimit).
            if (pe.configured && pfi.isCacheMiss()) {
                for (Addr line = blockAddress(addr); line < addr + size;
                     line += blkSize) {
                    if (!samePage(line, addr)) {
                        continue;
                    }
                    const Addr line_pa = pfi.getPaddr() + (line - addr);
                    if (cache.inCache(line_pa, is_secure)) {
                        continue; // resident: no fill will come
                    }
                    registerCapture(line_pa, line, pc, is_secure, true,
                                    clampBytes(announcedLimit(pc, line),
                                               line));
                }
            }
        }
    }

    // Targets first, then the stream round with the remaining room.
    if (!streamOnly) {
        drainEmission(addresses);
    }
    emitStreamRound(addresses);
}

PacketPtr
VHybrid::getPacket()
{
    // Range-based capture against the ANNOUNCED streams: a departing
    // prefetch whose VA falls inside a live STT window is a stream
    // line; if that stream's last-updating PC is a chain-table
    // producer, it is INDEX data — publish its physical page for
    // stream-aware replacement and register it for fill capture.
    // (Same peek-before-delegate as vector_tyche.cc: the DeferredPacket
    // still holds the VA, its pkt the translated PA.)
    if (!streamOnly && !pfq.empty() && pfq.front().pkt != nullptr) {
        const DeferredPacket &dp = pfq.front();
        const Addr line_va = blockAddress(dp.pfInfo.getAddr());
        const Addr line_pa = blockAddress(dp.pkt->getAddr());
        const bool secure = dp.pfInfo.isSecure();
        for (const auto &e : streamTbl->entries()) {
            if (!e.valid || line_va < blockAddress(e.currentAddr) ||
                line_va >= e.prefetchedUpTo) {
                continue;
            }
            const VectorChainTable::ProducerInfo info =
                chainTbl->producerInfo(e.lastPc);
            if (info.found) {
                chainTbl->registerStreamPage(line_pa);
                auto it = pipeTable.find(e.lastPc);
                if (it != pipeTable.end() && it->second.configured) {
                    // The announcing entry is in hand: its limit@ is
                    // the array end, and bytes past it are not index
                    // data (the last line straddles it).
                    registerCapture(line_pa, line_va, e.lastPc, secure,
                                    false,
                                    clampBytes(e.limitAddr, line_va));
                }
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
VHybrid::notifyFill(const CacheAccessProbeArg &acc)
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
    indexRoutingTable.erase(it);

    auto pe_it = pipeTable.find(rec.producerPC);
    if (pe_it == pipeTable.end() || !pe_it->second.configured) {
        return;
    }
    PipeEntry &pe = pe_it->second;
    if (pe.form.gen != chainTbl->generation()) {
        vhybridStats.staleConfigs++;
        return;
    }
    if (pkt->getSize() < blkSize) {
        return;
    }

    if (pe.sliceBuffer.size() >= sliceBufferEntries) {
        vhybridStats.bufferBusyDrops++;
        return;
    }

    CapturedLine cap;
    const uint8_t *data = pkt->getConstPtr<uint8_t>();
    cap.data.assign(data, data + pkt->getSize());
    cap.lineVaddr = blockAddress(rec.lineVaddr);
    cap.validBytes = rec.validBytes;
    pe.sliceBuffer.push_back(std::move(cap));
    vhybridStats.fillsCaptured++;
    DPRINTF(VHybrid, "fill captured: index line VA %#x (producer %#x)\n",
            rec.lineVaddr, rec.producerPC);
    scheduleDrain();
}

} // namespace prefetch
} // namespace gem5
