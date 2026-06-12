#include "mem/vector_splitter.hh"

#include "base/logging.hh"
#include "base/trace.hh"
#include "debug/VectorSplitter.hh"
#include "mem/rvv_ext.hh"
#include "sim/system.hh"

namespace gem5
{

VectorSplitter::VectorSplitter(const Params &params)
    : SimObject(params),
      cpuSidePort(name() + ".cpu_side_port", *this),
      scalarSidePort(name() + ".scalar_side_port", *this),
      vectorSidePort(name() + ".vector_side_port", *this),
      blockSize(params.system->cacheLineSize()),
      stats(this)
{
}

void
VectorSplitter::init()
{
    if (!cpuSidePort.isConnected() || !scalarSidePort.isConnected() ||
        !vectorSidePort.isConnected())
        fatal("VectorSplitter %s is not connected on all three ports\n",
              name());
}

Port &
VectorSplitter::getPort(const std::string &if_name, PortID idx)
{
    if (if_name == "cpu_side_port") {
        return cpuSidePort;
    } else if (if_name == "scalar_side_port") {
        return scalarSidePort;
    } else if (if_name == "vector_side_port") {
        return vectorSidePort;
    } else {
        return SimObject::getPort(if_name, idx);
    }
}

bool
VectorSplitter::isVectorAccess(PacketPtr pkt) const
{
    auto ext = pkt->req->getExtension<RVVExtension>();
    return ext && ext->isVector();
}

VectorSplitter::SplitterRequestPort &
VectorSplitter::routeTo(PacketPtr pkt)
{
    return isVectorAccess(pkt) ? vectorSidePort : scalarSidePort;
}

bool
VectorSplitter::recvTimingReq(PacketPtr pkt)
{
    assert(pkt->isRequest());

    // Classify before sending: a successful sendTimingReq hands the
    // packet to the receiver, which may modify or delete it.
    auto ext = pkt->req->getExtension<RVVExtension>();
    const bool tagged = (ext != nullptr);
    const bool is_vector = tagged && ext->isVector();
    const Addr line = pkt->getBlockAddr(blockSize);
    const bool needs_resp = pkt->needsResponse();

    // Same-line program order across the two hierarchies: stall any
    // request whose line still has an in-flight access on the other
    // side, and retry it once that response drains.
    auto it = outstanding.find(line);
    if (it != outstanding.end() && it->second.first != is_vector) {
        DPRINTF(VectorSplitter, "Stalling %s: line %#x outstanding on "
                "the %s side\n", pkt->print(), line,
                it->second.first ? "vector" : "scalar");
        stats.conflictStalls++;
        conflictRetryPending = true;
        return false;
    }

    DPRINTF(VectorSplitter, "Steering %s to %s side\n", pkt->print(),
            is_vector ? "vector" : "scalar");

    SplitterRequestPort &out = is_vector ? vectorSidePort : scalarSidePort;
    bool successful = out.sendTimingReq(pkt);

    if (successful) {
        if (needs_resp) {
            auto &entry = outstanding[line];
            entry.first = is_vector;
            entry.second++;
        }
        if (!tagged)
            stats.untaggedReqs++;
        else if (is_vector)
            stats.vectorReqs++;
        else
            stats.scalarReqs++;
    }
    return successful;
}

bool
VectorSplitter::recvTimingResp(PacketPtr pkt, SplitterRequestPort &from)
{
    assert(pkt->isResponse());

    // Read the line address before forwarding: the CPU owns the packet
    // once sendTimingResp succeeds.
    const Addr line = pkt->getBlockAddr(blockSize);

    bool successful = cpuSidePort.sendTimingResp(pkt);

    if (!successful) {
        // The O3 LSQ dcache port always accepts responses, so a second
        // refusal while one retry is pending cannot happen; a single
        // pending slot keeps the splitter stateless beyond this.
        panic_if(respRetryPort && respRetryPort != &from,
                 "%s: response refused on both mem-side ports; the "
                 "CPU-side peer must accept timing responses\n", name());
        respRetryPort = &from;
        return false;
    }

    auto it = outstanding.find(line);
    if (it != outstanding.end() && --it->second.second == 0) {
        outstanding.erase(it);
        // A request stalled on a cross-side conflict can be retried now
        // that a line has drained. The retry is sent after the response
        // is fully processed; if the resent request conflicts on a
        // different line it simply stalls again.
        if (conflictRetryPending) {
            conflictRetryPending = false;
            cpuSidePort.sendRetryReq();
        }
    }
    return true;
}

void
VectorSplitter::recvReqRetry()
{
    // Only the mem-side port that refused a request sends a retry, and
    // the CPU blocks after a refusal, so at most one is in flight.
    cpuSidePort.sendRetryReq();
}

void
VectorSplitter::recvRespRetry()
{
    panic_if(!respRetryPort,
             "%s: response retry from CPU side with none pending\n",
             name());
    SplitterRequestPort *port = respRetryPort;
    respRetryPort = nullptr;
    port->sendRetryResp();
}

void
VectorSplitter::recvTimingSnoopReq(PacketPtr pkt)
{
    // Snoops from either downstream hierarchy go up to the CPU, which
    // uses them for LL/SC tracking and load-order checks.
    cpuSidePort.sendTimingSnoopReq(pkt);
}

bool
VectorSplitter::recvTimingSnoopResp(PacketPtr pkt)
{
    panic("%s: unexpected timing snoop response from the CPU side; the "
          "cpu_side_port peer must be a CPU port, not a cache\n", name());
}

void
VectorSplitter::recvRetrySnoopResp()
{
    panic("%s: unexpected snoop response retry; the splitter never sends "
          "snoop responses downstream\n", name());
}

void
VectorSplitter::recvFunctional(PacketPtr pkt)
{
    routeTo(pkt).sendFunctional(pkt);
}

void
VectorSplitter::recvFunctionalSnoop(PacketPtr pkt)
{
    cpuSidePort.sendFunctionalSnoop(pkt);
}

Tick
VectorSplitter::recvAtomic(PacketPtr pkt)
{
    return routeTo(pkt).sendAtomic(pkt);
}

Tick
VectorSplitter::recvAtomicSnoop(PacketPtr pkt)
{
    return cpuSidePort.sendAtomicSnoop(pkt);
}

bool
VectorSplitter::tryTiming(PacketPtr pkt)
{
    return routeTo(pkt).tryTiming(pkt);
}

AddrRangeList
VectorSplitter::getAddrRanges() const
{
    // Both downstream paths reach the same memory bus and therefore
    // advertise identical ranges; report the scalar side's.
    return scalarSidePort.getAddrRanges();
}

bool
VectorSplitter::isSnooping() const
{
    return cpuSidePort.isSnooping();
}

void
VectorSplitter::recvRangeChange()
{
    cpuSidePort.sendRangeChange();
}

VectorSplitter::SplitterStats::SplitterStats(statistics::Group *parent)
    : statistics::Group(parent),
      ADD_STAT(scalarReqs, statistics::units::Count::get(),
               "Timing requests steered to the scalar hierarchy"),
      ADD_STAT(vectorReqs, statistics::units::Count::get(),
               "Timing requests steered to the vector hierarchy"),
      ADD_STAT(untaggedReqs, statistics::units::Count::get(),
               "Requests with no RVVExtension, steered scalar (expected 0)"),
      ADD_STAT(conflictStalls, statistics::units::Count::get(),
               "Requests stalled because their line had an in-flight "
               "access in the other hierarchy")
{
}

} // namespace gem5
