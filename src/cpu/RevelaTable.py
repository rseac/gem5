from m5.params import *
from m5.SimObject import SimObject


class RevelaStreamTable(SimObject):
    """Shared CPU-to-prefetcher channel of the ReVeLA prefetcher
    (--prefetcher revela): the Stream Tracking Table (STT) plus the
    Requested-Vector-Length shadow register.

    Vector Length Agnostic codes reveal each stream's full extent in
    advance: the AVL operand of vsetvl (the Requested Vector Length,
    RVL) is the element count the strip-mined loop still has to
    process, while the granted vl (GVL) is what one instruction can
    cover. The O3 issue stage snoops the AVL at every vset{i}vl{i};
    the LSQ reports every unit-stride vector load/store at translation
    finish, and this table tracks the stream [access, access + RVL)
    the access belongs to. The ReVeLA prefetcher walks the entries
    every cycle and prefetches ahead of current@ toward limit@.

    Attach one instance per core to both BaseCPU.revela_table and
    RevelaPrefetcher.stream_table. See cpu/revela_table.hh and
    "Exploiting Vector Code Semantics for Efficient Data Cache
    Prefetching" (Martinez Palau et al., ICS'24)."""

    type = "RevelaStreamTable"
    cxx_header = "cpu/revela_table.hh"
    cxx_class = "gem5::RevelaStreamTable"

    stt_entries = Param.Unsigned(
        16,
        "Stream Tracking Table entries (paper default 16; a 4-entry "
        "table already reaches 91% of the benefit)",
    )
    lru_max = Param.Unsigned(
        255,
        "Inactivity bound: an entry whose LRU counter reaches this "
        "value is invalidated as stale (8-bit counters in the paper)",
    )
