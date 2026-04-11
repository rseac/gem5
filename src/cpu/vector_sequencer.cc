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
    // Always return true to keep the pipeline moving.
    // We will apply the delay via latency instead of stalling.
    return true;
}

Cycles
VectorSequencer::getIssueDelay()
{
    Tick now = curTick();
    Tick start_tick = std::max(now, nextIdAvailableTick);
    
    // Update next availability (7 cycle floor)
    nextIdAvailableTick = start_tick + cpu->clockEdge(Cycles(7));
    
    // Return how many cycles this instruction was "delayed" by the dispatcher
    return cpu->ticksToCycles(start_tick - now);
}

} // namespace gem5
