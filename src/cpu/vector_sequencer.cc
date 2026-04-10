#include "cpu/vector_sequencer.hh"

namespace gem5
{

VectorSequencer::VectorSequencer(const Params &p)
    : SimObject(p),
      insnQueueSize(p.insnQueueSize),
      numLanes(p.numLanes),
      scoreboard(32, 0),
      inFlightCount(0)
{
}

bool
VectorSequencer::canIssue() const
{
    return inFlightCount < insnQueueSize;
}

void
VectorSequencer::dispatchInsn(OpClass op_class, int vsew, int vl, uint64_t seq_num)
{
    inFlightCount++;
}

void
VectorSequencer::retireInsn(uint64_t seq_num)
{
    if (inFlightCount > 0) {
        inFlightCount--;
    }
}

} // namespace gem5
