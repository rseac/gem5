from m5.params import *
from m5.SimObject import SimObject


class GdpChainTable(SimObject):
    """Shared CPU-to-prefetcher channel of the Gather Dataflow
    Prefetcher (--prefetcher gdp), built on Tyche's skeleton.
    Holds the two CPU-side structures: the
    Propagation Table (PT, per vector arch register) and the
    Dependency Chain Table (DCT, vector-instruction chain links with
    their opcodes and scalar operands; gather-link state — consumer
    pointer and EEW — lives in the head rows). Forward propagation
    runs at dispatch; backward propagation (the consumer-link write)
    runs at the gather's dispatch; scalar operands and gather bases
    are snooped at issue. Pipeline configurations are walked on demand
    when the prefetcher adopts a producer — the table stores no
    latched copy. Attach one instance per core to both
    BaseCPU.gdp_table and GDPPrefetcher.link_table.
    See cpu/gdp_table.hh."""

    type = "GdpChainTable"
    cxx_header = "cpu/gdp_table.hh"
    cxx_class = "gem5::GdpChainTable"

    dct_entries = Param.Unsigned(
        8, "Dependency Chain Table entries (whole table clears when "
        "full; gather-link state lives in the head rows)"
    )
    max_transform_stages = Param.Unsigned(
        4, "Maximum transform links between producer and gather (the "
        "replay pipeline's generic stage count; longer chains do not "
        "link)"
    )
