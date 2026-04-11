#ifndef __CPU_VECTOR_SEQUENCER_HH__
#define __CPU_VECTOR_SEQUENCER_HH__

#include "params/VectorSequencer.hh"
#include "sim/sim_object.hh"
#include "base/types.hh"

namespace gem5
{

class BaseCPU;

class VectorSequencer : public SimObject
{
  protected:
    int insnQueueSize;
    int numLanes;
    
    // Pointer back to the CPU
    BaseCPU* cpu;

    // Track when the dispatcher is free to accept the NEXT instruction
    Tick nextIdAvailableTick;

  public:
    typedef VectorSequencerParams Params;
    VectorSequencer(const Params &p);

    void setCPU(BaseCPU* _cpu) { cpu = _cpu; }

    // Handshake with the O3 CPU
    bool canIssue() const;
    Cycles getIssueDelay();
};

} // namespace gem5

#endif // __CPU_VECTOR_SEQUENCER_HH__
