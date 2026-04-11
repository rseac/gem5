#ifndef __CPU_VECTOR_SEQUENCER_HH__
#define __CPU_VECTOR_SEQUENCER_HH__

#include <deque>
#include <vector>

#include "cpu/o3/dyn_inst.hh"
#include "params/VectorSequencer.hh"
#include "sim/eventq.hh"
#include "sim/sim_object.hh"

namespace gem5
{

namespace o3 {
    class InstructionQueue;
    class IEW;
}

class VectorSequencer : public SimObject
{
  protected:
    int insnQueueSize;
    int numLanes;
    
    // Pointers back to the CPU and stages to signal completion
    BaseCPU* cpu;
    o3::InstructionQueue* iq;
    o3::IEW* iew;

    // Track when the sequencer is free to issue the NEXT instruction
    Tick nextIdAvailableTick;

    // We use a custom event class to handle multiple in-flight instructions
    class SequencerEvent : public Event
    {
      public:
        enum EventType { WakeDependents, RetireInsn };

        VectorSequencer *sequencer;
        o3::DynInstPtr inst;
        EventType type;

        SequencerEvent(VectorSequencer *_seq, o3::DynInstPtr _inst, EventType _type)
            : Event(Default_Pri, AutoDelete), 
              sequencer(_seq), inst(_inst), type(_type) {}

        void process() override;
        const char *description() const override { return "Vector Sequencer Event"; }
    };

    // Keep track of instructions currently being processed for ROB/Commit
    int inFlightCount;

  public:
    typedef VectorSequencerParams Params;
    VectorSequencer(const Params &p);

    void setCPU(BaseCPU* _cpu) { cpu = _cpu; }
    void setIQ(o3::InstructionQueue* _iq) { iq = _iq; }
    void setIEW(o3::IEW* _iew) { iew = _iew; }

    // Handshake with the O3 CPU
    bool canIssue() const;
    void dispatchInsn(o3::DynInstPtr inst, Cycles occupancy, Cycles readiness);
    
    // Called by the event process()
    void handleEvent(o3::DynInstPtr inst, SequencerEvent::EventType type);
};

} // namespace gem5

#endif // __CPU_VECTOR_SEQUENCER_HH__
