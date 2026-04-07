#include "cpu/latency_model.hh"

#include <algorithm>

namespace gem5
{

LatencyModel::LatencyModel(const Params &p)
    : SimObject(p),
      latenciesEW8(p.latenciesEW8),
      latenciesEW16(p.latenciesEW16),
      latenciesEW32(p.latenciesEW32),
      latenciesEW64(p.latenciesEW64),
      dispatchFloor(p.dispatchFloor)
{
}

Cycles
LatencyModel::getLatency(OpClass op_class, int vsew, int microVl) const
{
    // This model provides the base functional unit pipeline depth
    // OR the per-element iterative latency for serial units.
    const std::vector<int> *table = nullptr;
    
    switch (vsew) {
      case 0: table = &latenciesEW8; break;
      case 1: table = &latenciesEW16; break;
      case 2: table = &latenciesEW32; break;
      case 3: table = &latenciesEW64; break;
      default: table = &latenciesEW32; break;
    }

    int pipe_val = 0;
    if (table && static_cast<size_t>(op_class) < table->size()) {
        pipe_val = (*table)[op_class];
    }

    // Handle serial units (Division/Sqrt)
    // These units are non-pipelined; occupancy is (Base Latency * elements).
    if (op_class == enums::SimdDiv || op_class == enums::SimdFloatDiv || 
        op_class == enums::SimdFloatSqrt) {
        
        // occupancy = iterative_cycles_per_element * microVl
        // Note: For gem5's O3 FU occupancy, we return the total cycles 
        // minus 1 (since the ISA adds 1 cycle issue/throughput logic).
        int total_iterative = pipe_val * microVl;
        return Cycles(total_iterative);
    }

    // For pipelined units, we return the raw pipeline depth.
    // The ISA (vector.hh) will add the dynamic throughput (ceil(vl/lanes)).
    return Cycles(pipe_val);
}

} // namespace gem5
