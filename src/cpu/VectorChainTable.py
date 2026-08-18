from m5.params import *
from m5.SimObject import SimObject


class VectorChainTable(SimObject):
    """Shared CPU-to-prefetcher channel for vector instruction
    dependency chains, built on Tyche's skeleton. Used by the gdp,
    vtyche and vtyche2 prefetchers (link_table), StreamDemoteLRURP
    (stream-page registry) and the demand-side LSQ registration path.
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
    BaseCPU.vector_chain_table and GDPPrefetcher.link_table.
    See cpu/vector_chain_table.hh."""

    type = "VectorChainTable"
    cxx_header = "cpu/vector_chain_table.hh"
    cxx_class = "gem5::VectorChainTable"

    dct_entries = Param.Unsigned(
        8, "Dependency Chain Table entries (whole table clears when "
        "full; gather-link state lives in the head rows)"
    )
    max_transform_stages = Param.Unsigned(
        4, "Maximum transform links between producer and gather (the "
        "replay pipeline's generic stage count; longer chains do not "
        "link)"
    )
    consumers_per_producer = Param.Unsigned(
        1,
        "Consumer-link slots per head row: how many distinct gathers "
        "one producer index load may feed (multi-way patterns like "
        "A[B[i]] and C[B[i]]). Each slot is ~4 bits (3-bit DCT "
        "pointer + empty encoding). At 1 (default), backward "
        "propagation keeps its historical overwrite — the "
        "last-dispatched gather wins and multi-way loops ping-pong "
        "the slot; at > 1 it is insert-if-absent, and a distinct "
        "gather beyond capacity is rejected "
        "(consumerSlotsExhausted). The config script copies this "
        "from the prefetcher's consumers_per_producer pf-param so "
        "the two stay in sync.",
    )
    demand_stream_pages = Param.Bool(
        False,
        "Register stream pages from DEMAND accesses: every unit-stride "
        "vector load/store publishes its translated physical page to "
        "the stream-page registry at LSQ translation finish. Lets "
        "StreamDemoteLRURP run without a vector-chain-table prefetcher (the "
        "registry is otherwise fed only by prefetch departures), and "
        "broadens classification from producer index arrays to every "
        "unit-stride-touched array.",
    )
    promoted_page_entries = Param.Unsigned(
        1024,
        "Promoted-page (unlearned) set capacity: pages a replacement "
        "policy promoted out of the stream class after observing "
        "cross-sweep reuse are blocked from re-registration, bounded "
        "FIFO. ~1024 x 30-bit page numbers ~= 4KB. Too small and "
        "iterative working sets larger than the window churn back "
        "into the stream class as entries age out.",
    )
    monotone_arm = Param.Bool(
        False,
        "Gate demand-side stream-page registration on monotone "
        "progress (Stream Direction Table): a unit-stride stream may "
        "register pages only after covering arm_distance bytes of "
        "virtual address space monotonically, and the first backward "
        "jump beyond backward_slack STICKY-REVOKES it (an access below "
        "the stream's high-water mark proves the sweep recurs, so its "
        "lines have reuse that demotion would destroy). Revocation "
        "also retroactively drops the stream's registered pages. "
        "Affects only the demand path (demand_stream_pages); "
        "prefetcher-side registrations are untagged and ungated.",
    )
    arm_distance = Param.Unsigned(
        4096,
        "monotone_arm: monotone VA bytes (= distinct stream lines x "
        "64) a stream must cover before its pages may register. "
        "VLEN-independent because it is a distance, not an access "
        "count.",
    )
    backward_slack = Param.Unsigned(
        1024,
        "monotone_arm: dead zone under the high-water mark absorbing "
        "out-of-order completion jitter; only accesses further back "
        "than this revoke. Revocation is irreversible, so the test "
        "needs slack.",
    )
