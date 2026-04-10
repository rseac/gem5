#ifndef __CPU_VECTOR_SEQUENCER_HH__
#define __CPU_VECTOR_SEQUENCER_HH__

#include "cpu/o3/dyn_inst.hh"
#include "params/VectorSequencer.hh"
#include "sim/eventq.hh"
#include "sim/sim_object.hh"

namespace gem5
{

class VectorSequencer : public SimObject
{
  protected:
    int insnQueueSize;
    int numLanes;
    
    // Pointer back to the CPU to signal completion
    BaseCPU* cpu;

    // Internal instruction queue holding instructions in flight
    std::deque<o3::DynInstPtr> pendingInsts;

    // Event to handle the completion of a vector instruction
    void completeInsn();
    EventFunctionWrapper completeEvent;

  public:
    typedef VectorSequencerParams Params;
    VectorSequencer(const Params &p);

    void setCPU(BaseCPU* _cpu) { cpu = _cpu; }

    // Handshake with the O3 CPU
    bool canIssue() const;
    void dispatchInsn(o3::DynInstPtr inst, Cycles latency);
};

} // namespace gem5

#endif // __CPU_VECTOR_SEQUENCER_HH__
