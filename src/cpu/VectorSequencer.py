from m5.params import *
from m5.SimObject import SimObject

class VectorSequencer(SimObject):
    type = 'VectorSequencer'
    cxx_header = "cpu/vector_sequencer.hh"
    cxx_class = 'gem5::VectorSequencer'

    # Depth of the instruction buffer between scalar and vector unit
    insnQueueSize = Param.Int(4, "Number of vector instructions that can be in flight")
    
    # Number of lanes (mirrors throughput)
    numLanes = Param.Int(2, "Number of hardware lanes")
