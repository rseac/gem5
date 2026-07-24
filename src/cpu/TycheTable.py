from m5.params import *
from m5.SimObject import SimObject


class TycheChainTable(SimObject):
    """Shared CPU-to-prefetcher channel of the Tyche prefetcher, a port
    of the dependency-chain indirect prefetcher from the Tyche artifact
    (ChampSim/LoongArch) to gem5/RISC-V. Holds the three CPU-side
    structures: the IP-stride table (IPT, trained by the cache-side
    prefetcher on observed accesses, read at dispatch to root chains),
    the per-register Propagation Table (PT), and the Dependency Chain
    Table (DCT, instruction chain links with trained constant operands).
    Attach one instance per core to both BaseCPU.tyche_table and
    TychePrefetcher.chain_table. See cpu/tyche_table.hh."""

    type = "TycheChainTable"
    cxx_header = "cpu/tyche_table.hh"
    cxx_class = "gem5::TycheChainTable"

    dct_entries = Param.Unsigned(
        24, "Dependency Chain Table entries (whole table clears when full)"
    )
    ipt_entries = Param.Unsigned(32, "IP-stride table entries")
    dense_threshold = Param.Unsigned(
        115,
        "A chain link is dense (replayable) when it executes more than "
        "this many times per 256 executions of its chain head",
    )
