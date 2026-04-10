#ifndef __CPU_VECTOR_SEQUENCER_HH__
#define __CPU_VECTOR_SEQUENCER_HH__

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
    
    // Pointers back to the CPU and IQ to signal completion
    BaseCPU* cpu;
    o3::InstructionQueue* iq;
    o3::IEW* iew;

    // Internal instruction queue holding instructions and their latencies
    struct PendingInsn {
        o3::DynInstPtr inst;
        Cycles latency;
    };
    std::deque<PendingInsn> pendingInsts;

    // Event to handle the completion of a vector instruction
    void completeInsn();
    EventFunctionWrapper completeEvent;

  public:
    typedef VectorSequencerParams Params;
    VectorSequencer(const Params &p);

    void setCPU(BaseCPU* _cpu) { cpu = _cpu; }
    void setIQ(o3::InstructionQueue* _iq) { iq = _iq; }
    void setIEW(o3::IEW* _iew) { iew = _iew; }

    // Handshake with the O3 CPU
    bool canIssue() const;
    void dispatchInsn(o3::DynInstPtr inst, Cycles latency);
};

} // namespace gem5

#endif // __CPU_VECTOR_SEQUENCER_HH__
