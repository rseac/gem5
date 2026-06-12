#ifndef __MEM_VECTOR_SPLITTER_HH__
#define __MEM_VECTOR_SPLITTER_HH__

#include <unordered_map>

#include "base/statistics.hh"
#include "mem/port.hh"
#include "params/VectorSplitter.hh"
#include "sim/sim_object.hh"

namespace gem5
{

/**
 * The VectorSplitter sits between a CPU's dcache port and two parallel
 * cache hierarchies, steering each data access by the type of the
 * instruction that created it: requests from vector instructions go out
 * vector_side_port, everything else out scalar_side_port. The decision
 * reads the RVVExtension that the O3 LSQ attaches to every data request
 * (LSQ::LSQRequest::addReq -> StaticInst::annotateMemRequest); requests
 * without the extension default to the scalar side.
 *
 * The port plumbing is modeled on CommMonitor: the splitter forwards
 * everything in the same call chain and adds no latency. Snoops arriving
 * from either downstream hierarchy are forwarded up to the CPU.
 *
 * Same-line ordering: the two hierarchies are independent coherence
 * agents, but they serve one thread, so same-address program order must
 * be preserved across them. If a program-earlier store misses on one
 * side while a program-later store to the same line hits on the other,
 * the later store becomes globally visible first and the earlier miss's
 * line-fill then overwrites it (observed with vectorized newlib: calloc
 * zeroing a Bigint with vse64 raced _Balloc's scalar header stores 8
 * cycles later and erased them). The splitter therefore tracks lines
 * with in-flight requests per side and stalls any request whose line is
 * still outstanding on the *other* side, retrying once that response
 * drains. Cross-side line sharing degrades to ping-pong instead of
 * corrupting; disjoint workloads never stall (see conflictStalls stat).
 *
 * Assumptions (checked with panics where possible):
 *  - The CPU-side peer is a CPU port, not a cache: it never sends timing
 *    snoop responses, and it accepts timing responses without retries
 *    (the O3 LSQ dcache port always returns true from recvTimingResp).
 *    At most one downstream response retry can therefore be pending.
 *  - Functional requests arriving on cpu_side_port are steered like
 *    timing requests; functional accesses from the system port do not
 *    pass through here (they reach both hierarchies as functional snoops
 *    via the memory bus).
 */
class VectorSplitter : public SimObject
{
  public:
    using Params = VectorSplitterParams;

    VectorSplitter(const Params &params);

    void init() override;

    Port &getPort(const std::string &if_name,
                  PortID idx=InvalidPortID) override;

  private:
    /**
     * Mem-side port template, one instance per downstream hierarchy.
     * All recv functions forward to the splitter, which sends out of
     * the CPU-side port.
     */
    class SplitterRequestPort : public RequestPort
    {
      public:
        SplitterRequestPort(const std::string &_name,
                            VectorSplitter &_splitter)
            : RequestPort(_name), splitter(_splitter)
        { }

      protected:
        void recvFunctionalSnoop(PacketPtr pkt)
        {
            splitter.recvFunctionalSnoop(pkt);
        }

        Tick recvAtomicSnoop(PacketPtr pkt)
        {
            return splitter.recvAtomicSnoop(pkt);
        }

        bool recvTimingResp(PacketPtr pkt)
        {
            return splitter.recvTimingResp(pkt, *this);
        }

        void recvTimingSnoopReq(PacketPtr pkt)
        {
            splitter.recvTimingSnoopReq(pkt);
        }

        void recvRangeChange()
        {
            splitter.recvRangeChange();
        }

        bool isSnooping() const
        {
            return splitter.isSnooping();
        }

        void recvReqRetry()
        {
            splitter.recvReqRetry();
        }

        void recvRetrySnoopResp()
        {
            splitter.recvRetrySnoopResp();
        }

      private:
        VectorSplitter &splitter;
    };

    /**
     * CPU-side port. All recv functions forward to the splitter, which
     * steers them to one of the two mem-side ports.
     */
    class SplitterResponsePort : public ResponsePort
    {
      public:
        SplitterResponsePort(const std::string &_name,
                             VectorSplitter &_splitter)
            : ResponsePort(_name), splitter(_splitter)
        { }

      protected:
        void recvFunctional(PacketPtr pkt)
        {
            splitter.recvFunctional(pkt);
        }

        Tick recvAtomic(PacketPtr pkt)
        {
            return splitter.recvAtomic(pkt);
        }

        bool recvTimingReq(PacketPtr pkt)
        {
            return splitter.recvTimingReq(pkt);
        }

        bool recvTimingSnoopResp(PacketPtr pkt)
        {
            return splitter.recvTimingSnoopResp(pkt);
        }

        AddrRangeList getAddrRanges() const
        {
            return splitter.getAddrRanges();
        }

        void recvRespRetry()
        {
            splitter.recvRespRetry();
        }

        bool tryTiming(PacketPtr pkt)
        {
            return splitter.tryTiming(pkt);
        }

      private:
        VectorSplitter &splitter;
    };

    /** Port facing the CPU dcache port */
    SplitterResponsePort cpuSidePort;

    /** Port facing the scalar L1D */
    SplitterRequestPort scalarSidePort;

    /** Port facing the vector L1D */
    SplitterRequestPort vectorSidePort;

    /**
     * Mem-side port whose response the CPU side refused and that is
     * owed a sendRetryResp. The O3 LSQ never refuses responses, so at
     * most one can be pending; recvTimingResp panics otherwise.
     */
    SplitterRequestPort *respRetryPort = nullptr;

    /** Cache line size, for mapping request addresses to lines. */
    const unsigned blockSize;

    /**
     * Lines with in-flight (request sent, response not yet returned)
     * accesses: line address -> (steered to vector side?, count).
     * Bounded by the downstream caches' MSHR capacity.
     */
    std::unordered_map<Addr, std::pair<bool, unsigned>> outstanding;

    /**
     * True when a request was refused because its line was outstanding
     * on the other side; the CPU is owed a sendRetryReq once a tracked
     * line drains.
     */
    bool conflictRetryPending = false;

    /** True if the request was created by a vector instruction. */
    bool isVectorAccess(PacketPtr pkt) const;

    /** The mem-side port a packet steers to. */
    SplitterRequestPort &routeTo(PacketPtr pkt);

    void recvFunctional(PacketPtr pkt);

    void recvFunctionalSnoop(PacketPtr pkt);

    Tick recvAtomic(PacketPtr pkt);

    Tick recvAtomicSnoop(PacketPtr pkt);

    bool recvTimingReq(PacketPtr pkt);

    bool recvTimingResp(PacketPtr pkt, SplitterRequestPort &from);

    void recvTimingSnoopReq(PacketPtr pkt);

    bool recvTimingSnoopResp(PacketPtr pkt);

    void recvRetrySnoopResp();

    AddrRangeList getAddrRanges() const;

    bool isSnooping() const;

    void recvReqRetry();

    void recvRespRetry();

    void recvRangeChange();

    bool tryTiming(PacketPtr pkt);

    /**
     * Steering counters. untaggedReqs counts requests with no
     * RVVExtension (steered scalar); it should stay 0 in O3 SE runs and
     * is a cheap sanity check that the LSQ annotation hook is intact.
     */
    struct SplitterStats : public statistics::Group
    {
        SplitterStats(statistics::Group *parent);

        statistics::Scalar scalarReqs;
        statistics::Scalar vectorReqs;
        statistics::Scalar untaggedReqs;
        statistics::Scalar conflictStalls;
    } stats;
};

} // namespace gem5

#endif // __MEM_VECTOR_SPLITTER_HH__
