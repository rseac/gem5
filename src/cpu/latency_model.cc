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
    const std::vector<int> *table = nullptr;
    
    switch (vsew) {
      case 0: table = &latenciesEW8; break;
      case 1: table = &latenciesEW16; break;
      case 2: table = &latenciesEW32; break;
      case 3: table = &latenciesEW64; break;
      default: table = &latenciesEW32; break;
    }

    int lat = 0;
    if (table && static_cast<size_t>(op_class) < table->size()) {
        lat = (*table)[op_class];
    }

    if (lat < dispatchFloor) {
        lat = dispatchFloor;
    }

    return Cycles(lat);
}

} // namespace gem5
