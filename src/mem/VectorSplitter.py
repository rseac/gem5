from m5.params import *
from m5.proxy import *
from m5.SimObject import SimObject


class VectorSplitter(SimObject):
    """Steers data accesses into one of two parallel cache hierarchies.

    Sits between a CPU's dcache port and two private L1D caches. Requests
    created by vector instructions (identified via the RVVExtension the O3
    LSQ attaches to every data request) are sent out vector_side_port;
    everything else, including requests without the extension, goes out
    scalar_side_port. Forwarding adds no latency, except that a request
    to a cache line with an in-flight access in the other hierarchy is
    stalled until that access drains, preserving same-address program
    order across the two hierarchies (see the conflictStalls stat).
    """

    type = "VectorSplitter"
    cxx_header = "mem/vector_splitter.hh"
    cxx_class = "gem5::VectorSplitter"

    system = Param.System(
        Parent.any, "System the splitter belongs to (for cache line size)"
    )

    cpu_side_port = ResponsePort("Connects to the CPU dcache port")
    scalar_side_port = RequestPort(
        "Mem-side port for non-vector (scalar) data accesses"
    )
    vector_side_port = RequestPort(
        "Mem-side port for vector data accesses"
    )
