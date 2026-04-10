#include "cpu/vector_sequencer.hh"

#include "cpu/base.hh"
#include "cpu/o3/iew.hh"
#include "cpu/o3/inst_queue.hh"
#include "debug/IQ.hh"

namespace gem5
{

VectorSequencer::VectorSequencer(const Params &p)
    : SimObject(p),
      insnQueueSize(p.insnQueueSize),
      numLanes(p.numLanes),
      cpu(nullptr),
      iq(nullptr),
      iew(nullptr),
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
    pendingInsts.push_back({inst, latency});

    if (!completeEvent.scheduled()) {
        schedule(completeEvent, cpu->clockEdge(latency));
    }
}

void
VectorSequencer::completeInsn()
{
    assert(!pendingInsts.empty());
    
    PendingInsn finished = pendingInsts.front();
    pendingInsts.pop_front();
    o3::DynInstPtr finished_inst = finished.inst;

    if (finished_inst->isSquashed()) {
        DPRINTF(IQ, "Vector instruction [sn:%llu] was squashed, ignoring completion\n",
                finished_inst->seqNum);
    } else {
        // 1. Functional execution (non-memory only)
        if (!finished_inst->isMemRef()) {
            if (!finished_inst->isExecuted() && finished_inst->getFault() == NoFault) {
                finished_inst->execute();
            }
            finished_inst->setExecuted();
        }

        // 2. Wake dependents
        if (iq) {
            iq->wakeDependents(finished_inst);
        } else {
            finished_inst->setCompleted();
        }

        // 3. Retirement Handshake
        if (iew) {
            iew->instToCommit(finished_inst);
            iew->activityThisCycle();
        }
        
        DPRINTF(IQ, "Vector instruction [sn:%llu] completed in sequencer\n",
                finished_inst->seqNum);
    }

    // 4. Sequential Modeling: Start the next instruction in the queue
    if (!pendingInsts.empty()) {
        // Schedule the next completion based on the next instruction's latency
        schedule(completeEvent, cpu->clockEdge(pendingInsts.front().latency));
    }
}

} // namespace gem5
