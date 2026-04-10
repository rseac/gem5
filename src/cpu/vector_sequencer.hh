#ifndef __CPU_VECTOR_SEQUENCER_HH__
#define __CPU_VECTOR_SEQUENCER_HH__

#include <deque>
#include <vector>

#include "base/types.hh"
#include "params/VectorSequencer.hh"
#include "sim/sim_object.hh"
#include "base/types.hh"
#include "cpu/op_class.hh"

namespace gem5
{

class BaseCPU;

class VectorSequencer : public SimObject
{
  protected:
    int insnQueueSize;
    int numLanes;
    
    // Simple scoreboard for vector registers v0-v31
    // Stores the sequence number of the instruction writing to the register.
    std::vector<uint64_t> scoreboard;

    // Internal instruction queue
    // In a real implementation, this would hold pointers to DynInst
    int inFlightCount;

  public:
    typedef VectorSequencerParams Params;
    VectorSequencer(const Params &p);

    // Handshake with the O3 CPU
    bool canIssue() const;
    void dispatchInsn(OpClass op_class, int vsew, int vl, uint64_t seq_num);
    
    // Mark an instruction as retired (release scoreboard)
    void retireInsn(uint64_t seq_num);
};

} // namespace gem5

#endif // __CPU_VECTOR_SEQUENCER_HH__
