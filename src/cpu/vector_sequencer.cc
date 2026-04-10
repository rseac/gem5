#include "cpu/vector_sequencer.hh"

#include "cpu/base.hh"
#include "debug/IQ.hh"

namespace gem5
{

VectorSequencer::VectorSequencer(const Params &p)
    : SimObject(p),
      insnQueueSize(p.insnQueueSize),
      numLanes(p.numLanes),
      cpu(nullptr),
      completeEvent([this]{ completeInsn(); }, name() + ".completeEvent")
{
}

bool
VectorSequencer::canIssue() const
{
    return pendingInsts.size() < insnQueueSize;
}

void
VectorSequencer::dispatchInsn(o3::DynInstPtr inst, Cycles latency)
{
    // Record when this instruction should finish
    // (In a real hardware sequencer, this would be an iterative process)
    pendingInsts.push_back(inst);

    if (!completeEvent.scheduled()) {
        schedule(completeEvent, cpu->clockEdge(latency));
    }
}

void
VectorSequencer::completeInsn()
{
    assert(!pendingInsts.empty());
    
    o3::DynInstPtr finished_inst = pendingInsts.front();
    pendingInsts.pop_front();

    // Notify the scalar core that the vector instruction is done.
    // In gem5 O3, instructions are typically marked as completed 
    // by the FUCompletion event, which we are simulating here.
    finished_inst->setCompleted();
    
    // In a more complex model, we would signal IEW to move this 
    // instruction to the commit queue. 
    
    DPRINTF(IQ, "Vector instruction [sn:%llu] completed in sequencer\n",
            finished_inst->seqNum);

    // If there are more instructions, schedule the next one.
    // (Simplification: assuming next one starts immediately)
    if (!pendingInsts.empty()) {
        // Here we would ideally recalculate the next instruction's latency
        schedule(completeEvent, cpu->nextCycle());
    }
}

} // namespace gem5
