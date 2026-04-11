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
      nextIdAvailableTick(0),
      inFlightCount(0)
{
}

bool
VectorSequencer::canIssue() const
{
    // The O3 CPU can issue to us as long as our buffer isn't full.
    return inFlightCount < insnQueueSize;
}

void
VectorSequencer::dispatchInsn(o3::DynInstPtr inst, Cycles occupancy, Cycles readiness)
{
    inFlightCount++;

    // Calculate when this instruction can "start" in the sequencer.
    // ARA enforces a dispatch floor between vector instructions.
    Tick start_tick = std::max(curTick(), nextIdAvailableTick);
    
    // The sequencer will be busy for 7 cycles (floor) before it can 
    // take the NEXT instruction.
    nextIdAvailableTick = start_tick + cpu->clockEdge(Cycles(7));

    // 1. Schedule WakeDependents (Chaining)
    // This allows consumer instructions to start streaming elements early.
    Tick wake_tick = start_tick + cpu->clockEdge(readiness) - curTick();
    auto wake_event = new SequencerEvent(this, inst, SequencerEvent::WakeDependents);
    schedule(wake_event, wake_tick);

    // 2. Schedule Retirement (Occupancy)
    // This defines when the instruction finally leaves the vector unit.
    Tick retire_tick = start_tick + cpu->clockEdge(occupancy) - curTick();
    auto retire_event = new SequencerEvent(this, inst, SequencerEvent::RetireInsn);
    schedule(retire_event, retire_tick);

    DPRINTF(IQ, "Sequencer: Dispatched [sn:%llu]. Wake at %lu, Retire at %lu\n",
            inst->seqNum, wake_tick, retire_tick);
}

void
VectorSequencer::SequencerEvent::process()
{
    sequencer->handleEvent(inst, type);
}

void
VectorSequencer::handleEvent(o3::DynInstPtr inst, SequencerEvent::EventType type)
{
    if (inst->isSquashed()) {
        if (type == SequencerEvent::RetireInsn) inFlightCount--;
        return;
    }

    if (type == SequencerEvent::WakeDependents) {
        // --- PHASE 1: WAKE ---
        // Functional execution happens here so results are ready for chaining.
        if (!inst->isExecuted() && inst->getFault() == NoFault) {
            inst->execute();
        }
        inst->setExecuted();

        if (iq) {
            iq->wakeDependents(inst);
        }
        DPRINTF(IQ, "Sequencer: Waking dependents for [sn:%llu]\n", inst->seqNum);
    } 
    else if (type == SequencerEvent::RetireInsn) {
        // --- PHASE 2: RETIRE ---
        if (iew) {
            iew->instToCommit(inst);
            iew->activityThisCycle();
        }
        inFlightCount--;
        DPRINTF(IQ, "Sequencer: Retired instruction [sn:%llu]\n", inst->seqNum);
    }
}

} // namespace gem5
