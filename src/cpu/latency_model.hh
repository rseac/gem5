#ifndef __CPU_LATENCY_MODEL_HH__
#define __CPU_LATENCY_MODEL_HH__

#include <vector>

#include "params/LatencyModel.hh"
#include "sim/sim_object.hh"
#include "base/types.hh"
#include "cpu/op_class.hh"

namespace gem5
{

class LatencyModel : public SimObject
{
  protected:
    std::vector<int> latenciesEW8;
    std::vector<int> latenciesEW16;
    std::vector<int> latenciesEW32;
    std::vector<int> latenciesEW64;
    int dispatchFloor;

  public:
    typedef LatencyModelParams Params;
    LatencyModel(const Params &p);

    virtual Cycles getLatency(OpClass op_class, int vsew, int microVl) const;
};

} // namespace gem5

#endif // __CPU_LATENCY_MODEL_HH__
