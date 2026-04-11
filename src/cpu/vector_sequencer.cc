#include "cpu/vector_sequencer.hh"

#include "cpu/base.hh"

namespace gem5
{

VectorSequencer::VectorSequencer(const Params &p)
    : SimObject(p),
      insnQueueSize(p.insnQueueSize),
      numLanes(p.numLanes),
      cpu(nullptr),
      nextIdAvailableTick(0)
{
}

bool
VectorSequencer::canIssue() const
{
    // The Dispatcher is a serial bottleneck.
    // It can only accept one instruction every 7 cycles.
    return (curTick() >= nextIdAvailableTick);
}

void
VectorSequencer::recordIssue()
{
    // Mark the dispatcher as busy for the next 7 cycles.
    // This pushes the bottleneck into the future CUMULATIVELY.
    nextIdAvailableTick = std::max(curTick(), nextIdAvailableTick) + cpu->clockEdge(Cycles(7));
}

} // namespace gem5
