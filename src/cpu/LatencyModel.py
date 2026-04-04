from m5.params import *
from m5.SimObject import SimObject

class LatencyModel(SimObject):
    type = 'LatencyModel'
    cxx_header = "cpu/latency_model.hh"
    cxx_class = 'gem5::LatencyModel'

    # Mapping of OpClass indices to cycle values for each SEW
    # Default to empty, meaning use standard fallback.
    latenciesEW8  = VectorParam.Int([], "Custom latencies for EW8")
    latenciesEW16 = VectorParam.Int([], "Custom latencies for EW16")
    latenciesEW32 = VectorParam.Int([], "Custom latencies for EW32")
    latenciesEW64 = VectorParam.Int([], "Custom latencies for EW64")
    
    # Global floor for any instruction issue (sequencer bottleneck)
    dispatchFloor = Param.Int(0, "Minimum cycles for any instruction dependency")
