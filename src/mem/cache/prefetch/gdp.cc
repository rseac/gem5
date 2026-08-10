/**
 * GDP (Gather Dataflow Prefetcher) implementation. See gdp.hh for
 * the design and cpu/gdp_table.hh for the CPU-side half.
 */

#include "mem/cache/prefetch/gdp.hh"

#include <algorithm>
#include <cstring>
#include <limits>

#include "base/logging.hh"
#include "base/trace.hh"
#include "debug/GDP.hh"
#include "params/GDPPrefetcher.hh"
#include "sim/byteswap.hh"

namespace gem5
{

namespace prefetch
{

GDP::GDP(const GDPPrefetcherParams &p)
  : Queued(p),
    tbl(p.link_table),
    streamingDistance(p.streaming_distance),
    streamOnly(p.stream_only),
    sliceBufferEntries(p.slice_buffer_entries),
    pipelines(p.pipelines),
    pipelinesPerGather(p.pipelines_per_gather),
    irtEntries(p.routing_entries),
    streamTrackingTable(),
    indexRoutingTable(),
    gdpStats(this)
{
    fatal_if(tbl == nullptr, "%s: no link_table set. GDP needs the "
             "GdpChainTable that is also attached to the CPU's "
             "gdp_table param (the config script wires both).", name());
    fatal_if(streamingDistance < 1, "streaming_distance must be >= 1");
    fatal_if(sliceBufferEntries < 1, "slice_buffer_entries must be >= 1");
    fatal_if(pipelines < 1, "pipelines must be >= 1");
}

void
GDP::resetLearnedState()
{
    // The GdpChainTable registers its own reset callback; only this
    // prefetcher's runtime state is cleared here.
    streamTrackingTable.clear();
    configuredCount = 0;
    indexRoutingTable.clear();
}

GDP::GDPStats::GDPStats(statistics::Group *parent)
  : statistics::Group(parent),
    ADD_STAT(chunksObserved, statistics::units::Count::get(),
        "chunk events at producer PCs"),
    ADD_STAT(streamCandidates, statistics::units::Count::get(),
        "index-array stream candidates emitted"),
    ADD_STAT(chainDispatches, statistics::units::Count::get(),
        "pipeline configurations adopted at trigger"),
    ADD_STAT(chainNotReady, statistics::units::Count::get(),
        "linked triggers whose chain was not yet snoopable"),
    ADD_STAT(pipelinesSaturated, statistics::units::Count::get(),
        "linked producers denied a pipeline (pipelines cap)"),
    ADD_STAT(capturesRegistered, statistics::units::Count::get(),
        "index lines registered for capture (prefetch-window path)"),
    ADD_STAT(capturesRegisteredMiss, statistics::units::Count::get(),
        "index lines registered for capture (demand-miss path)"),
    ADD_STAT(fillsCaptured, statistics::units::Count::get(),
        "captured index-line fills replayed"),
    ADD_STAT(bufferBusyDrops, statistics::units::Count::get(),
        "fills dropped with the slice buffer full"),
    ADD_STAT(staleConfigs, statistics::units::Count::get(),
        "fills dropped on a stale configuration (table cleared)"),
    ADD_STAT(elementsReplayed, statistics::units::Count::get(),
        "elements replayed through the pipeline"),
    ADD_STAT(targetsGenerated, statistics::units::Count::get(),
        "indirect target candidates generated"),
    ADD_STAT(targetsFiltered, statistics::units::Count::get(),
        "targets dropped by the canonical-address filter"),
    ADD_STAT(stalenessAborts, statistics::units::Count::get(),
        "slice buffers flushed: walk cursor passed their line"),
    ADD_STAT(replayDeferred, statistics::units::Count::get(),
        "drain events paused by a full queue (the stall firing)"),
    ADD_STAT(replayWidthLimited, statistics::units::Count::get(),
        "drain events where a gather's replay pipelines ran out with "
        "elements still buffered")
{
}

void
GDP::streamAhead(SttEntry &ps, Addr addr, unsigned size,
                  std::vector<AddrPriority> &addresses)
{
    // Never gated: stream candidates are regenerable state and the
    // engine's fuel (index lines feed every capture). Gating them by
    // queue room proved to be a positive-feedback collapse — the
    // queue drains via cache pulls, which starve exactly when misses
    // rise, which is exactly when the stream is most needed.
    const Addr from = addr + size;
    const Addr to = addr + size * (Addr)(streamingDistance + 1);
    Addr line = blockAddress(from);
    for (; line < to; line += blkSize) {
        if (line >= ps.limitAddr) {
            addresses.push_back(AddrPriority(line, 0));
            gdpStats.streamCandidates++;
        }
    }
    if (line > ps.limitAddr) {
        ps.limitAddr = line;
    }
}

void
GDP::registerCapture(Addr line_pa, Addr line_va, Addr producer_pc,
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
        gdpStats.capturesRegisteredMiss++;
    } else {
        gdpStats.capturesRegistered++;
    }
}

Addr
GDP::applyChain(const GdpChainTable::ChainSnapshot &cfg,
                 uint64_t value) const
{
    using VOp = GdpChainTable::VOp;
    for (const auto &s : cfg.ops) {
        switch (s.op) {
          case VOp::SExt: {
            const unsigned sh = 64 - s.extFromBits;
            value = (uint64_t)((int64_t)(value << sh) >> sh);
            break;
          }
          case VOp::ZExt:
            if (s.extFromBits < 64) {
                value &= (1ULL << s.extFromBits) - 1;
            }
            break;
          case VOp::Sll: value <<= (s.scalar & 0x3f); break;
          case VOp::Srl: value >>= (s.scalar & 0x3f); break;
          case VOp::Sra:
            value = (uint64_t)((int64_t)value >> (s.scalar & 0x3f));
            break;
          case VOp::Add: value += s.scalar; break;
          case VOp::Sub: value -= s.scalar; break;
          case VOp::Rsub: value = s.scalar - value; break;
          case VOp::And: value &= s.scalar; break;
          case VOp::Or: value |= s.scalar; break;
          case VOp::Xor: value ^= s.scalar; break;
          case VOp::Mul: value *= s.scalar; break;
          default: break;
        }
    }
    // The dedicated base adder at the end of the pipeline.
    return cfg.base + value;
}

void
GDP::drainReplay(std::vector<AddrPriority> &addresses)
{
    // Emission budget: free queue slots, but never less than a small
    // floor. The floor models the continuous queue drain real
    // hardware has — gem5's queue empties only on cache pulls, which
    // starve under high miss rates; a zero budget there wedges the
    // drain until the staleness abort discards everything (observed
    // as the v2.1 poisson3Db collapse). Bounded displacement at the
    // floor keeps the pipeline moving; the abort keeps it fresh.
    int room = (int)queueSize - (int)pfq.size()
             - (int)pfqMissingTranslation.size()
             - (int)addresses.size();
    if (room < drainFloor) {
        room = drainFloor;
    }
    // Replay width: elements one gather's pipelines advance per drain
    // event, one per pipeline. Unlike `room` (the shared prefetch
    // queue) the pipelines are private to a gather, so this budget is
    // re-armed per producer below and running out never stops another
    // producer's drain.
    const int laneBudget = pipelinesPerGather
                         ? (int)pipelinesPerGather
                         : std::numeric_limits<int>::max();
    bool blocked = false;
    bool width_limited = false;
    for (auto &kv : streamTrackingTable) {
        if (blocked) {
            break;
        }
        SttEntry &ps = kv.second;
        int lanes = laneBudget;
        while (!ps.sliceBuffer.empty()) {
            CapturedLine &buf = ps.sliceBuffer.front();
            // Config integrity across the capture->drain window (the
            // lazy stand-in for the hardware invalidate pulse).
            if (!ps.configured || ps.config.gen != tbl->generation()) {
                gdpStats.staleConfigs++;
                ps.sliceBuffer.pop_front();
                continue;
            }
            // Staleness abort: the walk cursor moved past this index
            // line, so its targets would issue late. Strict <, so
            // demand-miss captures of the CURRENT chunk's own lines
            // survive their capture window.
            if (ps.valid && buf.lineVaddr < blockAddress(ps.lastAddr)) {
                gdpStats.stalenessAborts++;
                ps.sliceBuffer.pop_front();
                continue;
            }
            const unsigned width = ps.elemBytes;
            if (width == 0 || width > 8) {
                ps.sliceBuffer.pop_front();
                continue;
            }
            if (room <= 0) {
                blocked = true;
                break; // the stall: payload waits latched
            }
            if (lanes <= 0) {
                width_limited = true;
                break; // every pipeline busy: resume next event
            }
            const unsigned elems = buf.data.size() / width;
            while (buf.nextElem < elems && room > 0 && lanes > 0) {
                // Extract at EEW, zero-extended into a 64-bit value;
                // explicit vsext/vzext links handle sign.
                uint64_t raw = 0;
                std::memcpy(&raw, buf.data.data() + buf.nextElem * width,
                            width);
                buf.nextElem++;
                // One pipeline consumed, whether or not this element
                // survives to become a prefetch: it went through the
                // transform chain either way.
                lanes--;
                gdpStats.elementsReplayed++;

                const Addr target = applyChain(ps.config, letoh(raw));
                if (target == 0 || (target & 0xffffff0000000000ULL)) {
                    gdpStats.targetsFiltered++;
                    continue;
                }
                const Addr line = blockAddress(target);
                if (std::find(buf.emitted.begin(), buf.emitted.end(),
                              line) != buf.emitted.end()) {
                    continue; // line-dedup within the payload
                }
                buf.emitted.push_back(line);
                addresses.push_back(AddrPriority(target, 0));
                gdpStats.targetsGenerated++;
                room--;
                DPRINTF(GDP, "target: %#x (base %#x)\n", target,
                        ps.config.base);
            }
            if (buf.nextElem >= elems) {
                ps.sliceBuffer.pop_front(); // fully drained: entry freed
            } else if (lanes <= 0) {
                // Width-bound, not queue-bound: this gather resumes
                // next event, but other gathers still drain now.
                width_limited = true;
                break;
            } else {
                blocked = true;
                break; // stalled mid-payload; resume next access
            }
        }
    }
    if (blocked) {
        gdpStats.replayDeferred++;
    }
    if (width_limited) {
        gdpStats.replayWidthLimited++;
    }
}

void
GDP::calculatePrefetch(const PrefetchInfo &pfi,
    std::vector<AddrPriority> &addresses,
    const CacheAccessor &cache)
{
    if (pfi.isWrite() || !pfi.hasPC()) {
        // No chunk-event processing, but the drain still runs: any
        // observed access carries the translation context the
        // emission path needs.
        drainReplay(addresses);
        return;
    }

    const Addr addr = pfi.getAddr();
    const unsigned size = pfi.getSize();
    const Addr pc = pfi.getPC();
    const bool is_secure = pfi.isSecure();

    const GdpChainTable::ProducerInfo info = tbl->producerInfo(pc);
    if (!info.found) {
        drainReplay(addresses);
        return;
    }

    SttEntry &ps = streamTrackingTable[pc];
    if (addr != ps.lastAddr || !ps.valid) {
        gdpStats.chunksObserved++;
        // A jump backwards is a restarted walk: re-prefetch.
        if (ps.valid && addr < ps.lastAddr) {
            ps.limitAddr = 0;
        }
        ps.lastAddr = addr;
        ps.valid = true;

        // Architectural stream (no confidence counter needed: the opcode said
        // unit-stride).
        streamAhead(ps, addr, size, addresses);

        // Adopt this producer's pipeline configuration. The table
        // walks the chain on demand (consumer->head, bounded) — this
        // STT entry's copy becomes the only latched one, modeling the
        // backprop walk distributing ops into the pipeline stage
        // registers. Operands read here are the freshest snooped
        // values (no latch to lag behind).
        if (!streamOnly && info.linked) {
            if (!ps.configured && configuredCount >= pipelines) {
                gdpStats.pipelinesSaturated++;
            } else {
                GdpChainTable::ChainSnapshot snap =
                    tbl->pipelineConfig(info.dctPtr);
                if (snap.valid) {
                    if (!ps.configured) {
                        configuredCount++;
                    }
                    ps.config = std::move(snap);
                    ps.configured = true;
                    ps.elemBytes = info.elemBytes;
                    gdpStats.chainDispatches++;
                } else {
                    gdpStats.chainNotReady++;
                }
            }
        }
    }

    // Demand-miss capture path: the walk's own index-array misses
    // carry both VA and PA — register their lines directly.
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

    // Drain captured payloads after the stream's emissions.
    drainReplay(addresses);
}

PacketPtr
GDP::getPacket()
{
    // Range-based capture: a prefetch issuing inside a configured
    // producer's stream window is index data — learn the PA that
    // notifyFill will see. The issued packet's request is PA-only
    // (createPkt, queued.cc), so peek the DeferredPacket BEFORE
    // delegating: its PrefetchInfo still holds the VA the candidate
    // was generated with, and its pkt holds the translated PA.
    // (If the queue was empty and Queued refills it from pending
    // translations inside getPacket, that packet escapes this peek;
    // its line can still be captured via the demand-miss path.)
    if (!pfq.empty() && pfq.front().pkt != nullptr) {
        const DeferredPacket &dp = pfq.front();
        const Addr line_va = blockAddress(dp.pfInfo.getAddr());
        const Addr line_pa = blockAddress(dp.pkt->getAddr());
        const bool secure = dp.pfInfo.isSecure();
        for (auto &kv : streamTrackingTable) {
            SttEntry &ps = kv.second;
            if (!ps.configured || !ps.valid) {
                continue;
            }
            const Addr win_lo = blockAddress(ps.lastAddr) + blkSize;
            if (line_va >= win_lo && line_va < ps.limitAddr) {
                registerCapture(line_pa, line_va, kv.first, secure,
                                false);
                break;
            }
        }
    }
    return Queued::getPacket();
}

void
GDP::notifyFill(const CacheAccessProbeArg &acc)
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
    // Lazy stand-in for the hardware invalidate pulse: a wholesale
    // table clear would flash-clear the configured bits; the sim
    // compares generations at the next use instead.
    if (ps.config.gen != tbl->generation()) {
        gdpStats.staleConfigs++; // table cleared under this config
        return;
    }
    if (pkt->getSize() < blkSize) {
        return;
    }

    // Capture: latch the payload into a free slice-buffer entry;
    // the drain happens demand-clocked through drainReplay, gated on
    // queue room. A fill arriving with the buffer full is dropped —
    // chunk-granular load shedding (see the header comment).
    if (ps.sliceBuffer.size() >= sliceBufferEntries) {
        gdpStats.bufferBusyDrops++;
        return;
    }
    CapturedLine buf;
    const uint8_t *data = pkt->getConstPtr<uint8_t>();
    buf.data.assign(data, data + pkt->getSize());
    buf.lineVaddr = blockAddress(rec.lineVaddr);
    ps.sliceBuffer.push_back(std::move(buf));

    gdpStats.fillsCaptured++;
    DPRINTF(GDP, "fill captured: index line VA %#x (producer %#x)\n",
            rec.lineVaddr, rec.producerPC);
}

} // namespace prefetch
} // namespace gem5
