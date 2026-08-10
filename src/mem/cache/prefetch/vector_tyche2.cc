/**
 * VTyche (Vector Tyche) implementation. See vector_tyche2.hh for the
 * design and cpu/gdp_table.hh for the CPU-side half it shares with GDP.
 */

#include "mem/cache/prefetch/vector_tyche2.hh"

#include <algorithm>
#include <cstring>

#include "base/bitfield.hh"
#include "base/intmath.hh"
#include "base/logging.hh"
#include "base/trace.hh"
#include "debug/VTyche.hh"
#include "params/VectorTyche2Prefetcher.hh"
#include "sim/byteswap.hh"

namespace gem5
{

namespace prefetch
{

VectorTyche2::VectorTyche2(const VectorTyche2PrefetcherParams &p)
  : Queued(p),
    tbl(p.link_table),
    streamingDistance(p.index_distance),
    releaseDistance(p.release_distance),
    releasePace(p.release_pace),
    streamStartAtDistance(p.stream_start_at_distance),
    streamOnly(p.stream_only),
    sliceBufferEntries(p.slice_buffer_entries),
    pipelines(p.pipelines),
    irtEntries(p.routing_entries),
    drainFloor(p.drain_floor),
    dedupBufferSize(p.dedup_buffer_size),
    drainPeriod(p.drain_period),
    streamTrackingTable(),
    indexRoutingTable(),
    vtycheStats(this),
    drainEvent([this] { drainTick(); }, name())
{
    fatal_if(tbl == nullptr, "%s: no link_table set. VTyche needs the "
             "GdpChainTable that is also attached to the CPU's "
             "gdp_table param (the config script wires both).", name());
    fatal_if(streamingDistance < 1, "index_distance must be >= 1");
    fatal_if(releaseDistance < 1, "release_distance must be >= 1");
    fatal_if(releaseDistance >= streamingDistance,
             "release_distance must be < index_distance (the near "
             "window must sit inside the deep window)");
    fatal_if(sliceBufferEntries < 1, "slice_buffer_entries must be >= 1");
    fatal_if(pipelines < 1, "pipelines must be >= 1");
    fatal_if(drainFloor < 0, "drain_floor must be >= 0");
}

void
VectorTyche2::resetLearnedState()
{
    // The GdpChainTable registers its own reset callback; only this
    // prefetcher's runtime state is cleared here.
    streamTrackingTable.clear();
    configuredCount = 0;
    indexRoutingTable.clear();
}

VectorTyche2::VTycheStats::VTycheStats(statistics::Group *parent)
  : statistics::Group(parent),
    ADD_STAT(chunksObserved, statistics::units::Count::get(),
        "chunk events at producer PCs"),
    ADD_STAT(streamCandidates, statistics::units::Count::get(),
        "index-array stream candidates emitted"),
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
    ADD_STAT(targetsGenerated, statistics::units::Count::get(),
        "indirect target candidates generated"),
    ADD_STAT(targetsFiltered, statistics::units::Count::get(),
        "targets dropped by the canonical-address filter"),
    ADD_STAT(targetsDeduplicated, statistics::units::Count::get(),
        "targets dropped as duplicate lines within one batch"),
    ADD_STAT(targetsCrossDeduplicated, statistics::units::Count::get(),
        "targets dropped: line emitted within the dedup window"),
    ADD_STAT(stalenessAborts, statistics::units::Count::get(),
        "batches flushed: walk cursor passed their index line"),
    ADD_STAT(emissionDeferred, statistics::units::Count::get(),
        "emission events paused by a full queue (the stall firing)"),
    ADD_STAT(emissionLineLimited, statistics::units::Count::get(),
        "events where the array's one-line conversion slot ended "
        "with further lines still buffered"),
    ADD_STAT(selfDrainTicks, statistics::units::Count::get(),
        "self-clocked drain firings that ran the emission engine"),
    ADD_STAT(promotionsIssued, statistics::units::Count::get(),
        "near-window re-promotions issued (just-in-time L1 copies)"),
    ADD_STAT(releaseHeld, statistics::units::Count::get(),
        "buffered lines held back by the release gate at a drain"),
    ADD_STAT(pacedStops, statistics::units::Count::get(),
        "drain firings ended early by the release-pace budget")
{
}

// ---------------------------------------------------------------------
// The collapse: chain -> base + (index << shift)
// ---------------------------------------------------------------------

VectorTyche2::LinearForm
VectorTyche2::collapse(const GdpChainTable::ChainSnapshot &snap) const
{
    using VOp = GdpChainTable::VOp;
    LinearForm f;
    f.gen = snap.gen;
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

Addr
VectorTyche2::applyForm(const LinearForm &f, uint64_t raw) const
{
    if (f.extBits > 0 && f.extBits < 64) {
        if (f.extSigned) {
            const unsigned sh = 64 - f.extBits;
            raw = (uint64_t)((int64_t)(raw << sh) >> sh);
        } else {
            raw &= (1ULL << f.extBits) - 1;
        }
    }
    // The lane: one shifter, one adder.
    return f.base + (raw << f.shift);
}

bool
VectorTyche2::inDedupWindow(const SttEntry &ps, Addr line) const
{
    for (const auto &prev : ps.dedupWindow) {
        if (std::find(prev.begin(), prev.end(), line) != prev.end()) {
            return true;
        }
    }
    return false;
}

// ---------------------------------------------------------------------
// Stream walk and capture registration (shared with GDP by design)
// ---------------------------------------------------------------------

void
VectorTyche2::streamAhead(SttEntry &ps, Addr addr, unsigned size,
                         std::vector<AddrPriority> &addresses)
{
    // Never gated on queue room: stream candidates are regenerable and
    // are the engine's fuel — every capture comes from one. Gating them
    // is a positive-feedback collapse, because the queue drains via
    // cache pulls, which starve exactly when misses rise.
    const Addr from = addr + size;
    const Addr to = addr + size * (Addr)(streamingDistance + 1);
    Addr line = blockAddress(from);
    if (streamStartAtDistance && ps.limitAddr < line) {
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
        if (line >= ps.limitAddr) {
            addresses.push_back(AddrPriority(line, 0));
            vtycheStats.streamCandidates++;
        }
    }
    if (line > ps.limitAddr) {
        ps.limitAddr = line;
    }

    // Near-promotion window [next line, cursor + (release+1) chunks):
    // re-emit lines the deep window staged long ago so their
    // just-in-time L1 copy is present for the demand access. Its own
    // high-water mark paces it; the queue filter drops the cold-start
    // overlap with the deep window's first burst.
    const Addr promo_to = addr + size * (Addr)(releaseDistance + 1);
    Addr pline = blockAddress(from);
    if (ps.promoteLimitAddr > pline) {
        pline = ps.promoteLimitAddr;
    }
    for (; pline < promo_to; pline += blkSize) {
        addresses.push_back(AddrPriority(pline, 0));
        vtycheStats.promotionsIssued++;
    }
    if (pline > ps.promoteLimitAddr) {
        ps.promoteLimitAddr = pline;
    }
}

void
VectorTyche2::registerCapture(Addr line_pa, Addr line_va, Addr producer_pc,
                             bool secure, bool from_miss)
{
    for (const IrtEntry &c : indexRoutingTable) {
        if (c.linePaddr == line_pa && c.secure == secure) {
            return; // already watched
        }
    }
    if (indexRoutingTable.size() >= irtEntries) {
        indexRoutingTable.pop_front();
    }
    indexRoutingTable.push_back({line_pa, line_va, producer_pc, secure});
    if (from_miss) {
        vtycheStats.capturesRegisteredMiss++;
    } else {
        vtycheStats.capturesRegistered++;
    }
}

// ---------------------------------------------------------------------
// Conversion (parallel) and emission (demand-clocked, stall-on-full)
// ---------------------------------------------------------------------

void
VectorTyche2::convertLine(SttEntry &ps, const CapturedLine &cap)
{
    const unsigned width = ps.elemBytes;
    if (width == 0 || width > 8) {
        return; // line consumed; the latch stays empty
    }
    const unsigned elems = cap.data.size() / width;

    ConvertedLine batch;
    batch.lineVaddr = cap.lineVaddr;
    batch.targets.reserve(elems);

    // One shift-and-add lane per element, all in the same event: the
    // whole line converts at once, which is the point of restricting
    // the chain to an affine form. GDP walks the same payload through
    // its replay pipeline at pipelines_per_gather elements per event
    // instead — the drain compute width is the entire structural
    // difference between the two designs.
    for (unsigned i = 0; i < elems; i++) {
        uint64_t raw = 0;
        std::memcpy(&raw, cap.data.data() + i * width, width);
        vtycheStats.elementsConverted++;

        const Addr target = applyForm(ps.form, letoh(raw));
        if (target == 0 || (target & 0xffffff0000000000ULL)) {
            vtycheStats.targetsFiltered++;
            continue;
        }
        const Addr line = blockAddress(target);
        bool dup = false;
        for (Addr t : batch.targets) {
            if (blockAddress(t) == line) {
                dup = true;
                break;
            }
        }
        if (dup) {
            vtycheStats.targetsDeduplicated++;
            continue;
        }
        batch.targets.push_back(target);
    }

    DPRINTF(VTyche, "converted index line VA %#x: %u elements -> %u "
            "distinct target lines (base %#x shift %u)\n", cap.lineVaddr,
            elems, (unsigned)batch.targets.size(), ps.form.base,
            ps.form.shift);
    if (batch.targets.empty()) {
        // Nothing to emit, but the line was processed: it still ages
        // the dedup window.
        if (dedupBufferSize) {
            ps.dedupWindow.push_back({});
            while (ps.dedupWindow.size() > dedupBufferSize) {
                ps.dedupWindow.pop_front();
            }
        }
        return;
    }
    ps.latch = std::move(batch);
    ps.latchValid = true;
}

void
VectorTyche2::drainEmission(std::vector<AddrPriority> &addresses)
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
    if (room < drainFloor) {
        room = drainFloor;
    }
    bool blocked = false;
    bool line_limited = false;
    unsigned paced_left = releasePace ? releasePace : ~0u;
    for (auto &kv : streamTrackingTable) {
        if (blocked) {
            break;
        }
        SttEntry &ps = kv.second;
        // The lane array is one line wide: per event a producer can
        // drain its output latch and convert AT MOST ONE buffered
        // line into it. Stale-line flushes stay free — they model the
        // broadside invalidate, not the array.
        bool converted = false;
        while (true) {
            if (ps.latchValid) {
                // Form integrity across the capture->emit window (the
                // lazy stand-in for the hardware invalidate pulse).
                if (!ps.configured ||
                    ps.form.gen != tbl->generation()) {
                    vtycheStats.staleConfigs++;
                    ps.latchValid = false;
                    continue;
                }
                // Staleness abort: the walk cursor moved past this
                // index line, so its targets would issue late.
                // Strict <, so demand-miss captures of the CURRENT
                // chunk's own lines survive their capture window.
                if (ps.valid &&
                    ps.latch.lineVaddr < blockAddress(ps.lastAddr)) {
                    vtycheStats.stalenessAborts++;
                    ps.latchValid = false;
                    continue;
                }
                if (room <= 0) {
                    blocked = true;
                    break; // the stall: finished addresses wait latched
                }
                while (ps.latch.next < ps.latch.targets.size() &&
                       room > 0 && paced_left > 0) {
                    const Addr target =
                        ps.latch.targets[ps.latch.next++];
                    if (dedupBufferSize) {
                        const Addr line = blockAddress(target);
                        if (inDedupWindow(ps, line)) {
                            // First-emit-wins: an earlier line inside
                            // the window already pushed this line.
                            vtycheStats.targetsCrossDeduplicated++;
                            continue; // no queue slot consumed
                        }
                        ps.latch.emitted.push_back(line);
                    }
                    addresses.push_back(AddrPriority(target, 0));
                    vtycheStats.targetsGenerated++;
                    room--;
                    paced_left--;
                    DPRINTF(VTyche, "target: %#x (base %#x shift "
                            "%u)\n", target, ps.form.base,
                            ps.form.shift);
                }
                if (ps.latch.next < ps.latch.targets.size()) {
                    if (paced_left == 0) {
                        vtycheStats.pacedStops++;
                    }
                    blocked = true;
                    break; // stalled mid-latch; resume next firing
                }
                if (dedupBufferSize) {
                    // The contribution enters the window even when
                    // empty: the window ages by processed lines.
                    ps.dedupWindow.push_back(std::move(ps.latch.emitted));
                    while (ps.dedupWindow.size() > dedupBufferSize) {
                        ps.dedupWindow.pop_front();
                    }
                }
                ps.latchValid = false; // fully emitted
                continue;
            }
            // Latch free: convert the next DUE buffered line into it.
            // The release gate holds a captured line until the walk
            // cursor is within releaseDistance chunks of it, so deep
            // metadata staging does not translate into early target
            // fetches (the decoupled-lead design point).
            if (ps.sliceBuffer.empty()) {
                break;
            }
            if (converted) {
                line_limited = true;
                break; // the array already ran this event
            }
            auto due = ps.sliceBuffer.end();
            if (ps.valid && ps.lastSize) {
                const Addr release_edge = blockAddress(
                    ps.lastAddr + (Addr)(releaseDistance + 1) * ps.lastSize);
                for (auto it = ps.sliceBuffer.begin();
                     it != ps.sliceBuffer.end(); ++it) {
                    if (it->lineVaddr < release_edge) {
                        due = it;
                        break;
                    }
                }
            } else {
                due = ps.sliceBuffer.begin();
            }
            if (due == ps.sliceBuffer.end()) {
                vtycheStats.releaseHeld++;
                break; // everything buffered is still ahead of the gate
            }
            const CapturedLine cap = std::move(*due);
            ps.sliceBuffer.erase(due);
            if (!ps.configured || ps.form.gen != tbl->generation()) {
                vtycheStats.staleConfigs++;
                continue; // flushed before any compute was paid
            }
            if (ps.valid && cap.lineVaddr < blockAddress(ps.lastAddr)) {
                vtycheStats.stalenessAborts++;
                continue; // flushed before any compute was paid
            }
            convertLine(ps, cap); // the array: whole line, one event
            converted = true;
        }
    }
    if (blocked) {
        vtycheStats.emissionDeferred++;
    }
    if (line_limited) {
        vtycheStats.emissionLineLimited++;
    }
}

// ---------------------------------------------------------------------
// Self-clocked drain
// ---------------------------------------------------------------------

bool
VectorTyche2::drainWorkPending() const
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
VectorTyche2::scheduleDrain()
{
    if (drainPeriod == 0 || drainEvent.scheduled() || !drainWorkPending()) {
        return;
    }
    schedule(drainEvent, clockEdge(drainPeriod));
}

void
VectorTyche2::drainTick()
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
    vtycheStats.selfDrainTicks++;
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
VectorTyche2::notify(const CacheAccessProbeArg &acc, const PrefetchInfo &pfi)
{
    // Latch this access's translation context for the self-clocked
    // drain. The request must carry what insert() dereferences: a VA
    // (useVirtualAddresses PA math) and a ContextID (cross-page
    // translation requests). The Request is a shared_ptr, so it
    // outlives the packet safely.
    if (drainPeriod != 0 && acc.pkt != nullptr && acc.pkt->req != nullptr &&
        acc.pkt->req->hasVaddr() && acc.pkt->req->hasContextId()) {
        drainCtxReq = acc.pkt->req;
        drainCtxPfi.reset(new PrefetchInfo(pfi, pfi.getAddr()));
        drainCtxCache = &acc.cache;
    }
    Queued::notify(acc, pfi);
    scheduleDrain();
}

void
VectorTyche2::calculatePrefetch(const PrefetchInfo &pfi,
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

    const GdpChainTable::ProducerInfo info = tbl->producerInfo(pc);
    if (!info.found) {
        drainEmission(addresses);
        return;
    }

    SttEntry &ps = streamTrackingTable[pc];
    if (addr != ps.lastAddr || !ps.valid) {
        vtycheStats.chunksObserved++;
        // A jump backwards is a restarted walk: re-prefetch. The dedup
        // window clears with it — its lines may have been evicted
        // since, and suppressing their re-emission would punch holes
        // in the restarted pass.
        if (ps.valid && addr < ps.lastAddr) {
            ps.limitAddr = 0;
            ps.dedupWindow.clear();
        }
        ps.lastAddr = addr;
        ps.valid = true;
        ps.lastSize = size;

        // Architectural stream (no confidence: the opcode said
        // unit-stride).
        streamAhead(ps, addr, size, addresses);

        // Adopt this producer's linear form. The table walks the chain
        // on demand (consumer->head, bounded); collapse() folds it into
        // (base, shift) here, so the runtime never sees the stages.
        // Operands read here are the freshest snooped values.
        if (!streamOnly && info.linked) {
            if (!ps.configured && configuredCount >= pipelines) {
                vtycheStats.pipelinesSaturated++;
            } else {
                const GdpChainTable::ChainSnapshot snap =
                    tbl->pipelineConfig(info.dctPtr);
                if (!snap.valid) {
                    vtycheStats.chainNotReady++;
                } else {
                    const LinearForm f = collapse(snap);
                    if (!f.valid) {
                        // Not an A[B[i]] shape. Reject rather than
                        // approximate: this is exactly the class of
                        // pattern GDP's replay pipeline exists for.
                        vtycheStats.chainsRejected++;
                    } else {
                        if (!ps.configured) {
                            configuredCount++;
                        }
                        ps.form = f;
                        ps.configured = true;
                        ps.elemBytes = info.elemBytes;
                        vtycheStats.formsAdopted++;
                        DPRINTF(VTyche, "PC %#x form adopted: base=%#x "
                                "shift=%u ext=%u/%s eew=%uB (%u lanes)\n",
                                pc, f.base, f.shift, f.extBits,
                                f.extSigned ? "s" : "u", info.elemBytes,
                                info.elemBytes ? blkSize / info.elemBytes
                                               : 0);
                    }
                }
            }
        }
    }

    // Demand-miss capture path: the walk's own index-array misses carry
    // both VA and PA — register their lines directly.
    if (!streamOnly && ps.configured && pfi.isCacheMiss()) {
        for (Addr line = blockAddress(addr); line < addr + size;
             line += blkSize) {
            if (!samePage(line, addr)) {
                continue;
            }
            const Addr line_pa = pfi.getPaddr() + (line - addr);
            if (cache.inCache(line_pa, is_secure)) {
                continue; // resident: no fill will come
            }
            registerCapture(line_pa, line, pc, is_secure, true);
        }
    }

    // Emit converted targets after the stream's own candidates.
    drainEmission(addresses);
}

PacketPtr
VectorTyche2::getPacket()
{
    // Range-based capture: a prefetch issuing inside a configured
    // producer's stream window is index data — learn the PA that
    // notifyFill will see. The issued packet's request is PA-only
    // (createPkt, queued.cc), so peek the DeferredPacket BEFORE
    // delegating: its PrefetchInfo still holds the VA the candidate was
    // generated with, and its pkt holds the translated PA.
    if (!pfq.empty() && pfq.front().pkt != nullptr) {
        const DeferredPacket &dp = pfq.front();
        const Addr line_va = blockAddress(dp.pfInfo.getAddr());
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
            // Near re-promotions (below the promotion mark) already
            // had their deep pass captured; registering again would
            // re-stage the line at release time. Deep departures
            // register as before (registerCapture dedups by PA).
            if (ps.configured && line_va >= ps.promoteLimitAddr) {
                registerCapture(line_pa, line_va, kv.first, secure, false);
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
VectorTyche2::notifyFill(const CacheAccessProbeArg &acc)
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

    auto ps_it = streamTrackingTable.find(rec.producerPC);
    if (ps_it == streamTrackingTable.end() || !ps_it->second.configured) {
        return;
    }
    SttEntry &ps = ps_it->second;
    // Lazy stand-in for the hardware invalidate pulse: a wholesale table
    // clear would flash-clear the configured bits; the sim compares
    // generations at the next use instead.
    if (ps.form.gen != tbl->generation()) {
        vtycheStats.staleConfigs++;
        return;
    }
    if (pkt->getSize() < blkSize) {
        return;
    }

    // A near re-promotion of an index line already staged produces a
    // second fill for the same line; converting it twice would emit
    // duplicate targets.
    for (const CapturedLine &c : ps.sliceBuffer) {
        if (c.lineVaddr == blockAddress(rec.lineVaddr)) {
            return; // already staged
        }
    }

    // Chunk-granular load shedding: a fill arriving with the slice
    // buffer full is dropped whole, because one uncovered line stalls
    // the gather anyway. With deep staging this is the backpressure
    // signal that index_distance has outrun slice_buffer_entries.
    if (ps.sliceBuffer.size() >= sliceBufferEntries) {
        vtycheStats.bufferBusyDrops++;
        return;
    }

    // Latch the raw payload, exactly as GDP does; conversion happens
    // at the drain, one line per event through the lane array.
    CapturedLine cap;
    const uint8_t *data = pkt->getConstPtr<uint8_t>();
    cap.data.assign(data, data + pkt->getSize());
    cap.lineVaddr = blockAddress(rec.lineVaddr);
    ps.sliceBuffer.push_back(std::move(cap));
    vtycheStats.fillsCaptured++;
    DPRINTF(VTyche, "fill captured: index line VA %#x (producer %#x)\n",
            rec.lineVaddr, rec.producerPC);
    // Fresh conversion work: arm the self-clocked drain.
    scheduleDrain();
}

} // namespace prefetch
} // namespace gem5
