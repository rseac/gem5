"""
Cache hierarchy with a second, parallel private L1D+L2 chain reserved for
vector memory accesses.

Topology per core (instruction side unchanged from the parent class):

                 core dcache_port
                        |
                  VectorSplitter
                  /            \\
        l1d-cache-N        vector-l1d-cache-N
             |                     |
         L2XBar                 L2XBar
             |                     |
         l2-cache-N        vector-l2-cache-N
              \\                   /
                membus (SystemXBar)

The VectorSplitter (src/mem/vector_splitter.cc) steers each data request
by the RVVExtension the O3 LSQ attaches to it: vector-instruction
accesses go to the vector chain, everything else to the scalar chain.
Both L2s sit on the coherent membus, so any line touched by both access
classes is kept coherent by ordinary snooping (it migrates between the
hierarchies instead of being duplicated).

The L1I and the page-table walker stay on the scalar chain, exactly as
in PrivateL1PrivateL2CacheHierarchy, which this class mirrors otherwise.
"""

from m5.objects import (
    L2XBar,
    VectorSplitter,
)
from m5.params import NULL

from gem5.components.boards.abstract_board import AbstractBoard
from gem5.components.cachehierarchies.abstract_cache_hierarchy import (
    AbstractCacheHierarchy,
)
from gem5.components.cachehierarchies.classic.caches.l1dcache import L1DCache
from gem5.components.cachehierarchies.classic.caches.l1icache import L1ICache
from gem5.components.cachehierarchies.classic.caches.l2cache import L2Cache
from gem5.components.cachehierarchies.classic.private_l1_private_l2_cache_hierarchy import (
    PrivateL1PrivateL2CacheHierarchy,
)
from gem5.isas import ISA
from gem5.utils.override import overrides


class VectorSplitCacheHierarchy(PrivateL1PrivateL2CacheHierarchy):
    """
    PrivateL1PrivateL2CacheHierarchy plus a parallel vector-only L1D+L2
    chain per core, fed through a VectorSplitter on the dcache port.

    The vector caches are the same stdlib L1DCache/L2Cache classes as the
    scalar side. Prefetching is *disabled on every cache by default* (the
    stdlib classes attach a StridePrefetcher; incorporate_cache overrides
    each one to NULL), then the optional prefetcher factory is attached to
    exactly one cache, chosen by the caller across both axes: scalar vs
    vector chain (scalar_*/vector_* argument) and L1D vs L2 (*_l1d/*_l2
    argument). The net effect is at most one active prefetcher, on the cache
    the caller selects, and none at all when no factory is passed.

    scalar_l1d_prefetcher / scalar_l2_prefetcher / vector_l1d_prefetcher /
    vector_l2_prefetcher are each a zero-arg factory (callable -> a fresh
    prefetcher SimObject); the factory is invoked once per core because a
    SimObject cannot be shared between caches. The config script only ever
    passes one of the four non-None. See rvv/prefetcher_factory.py.

    prefetcher_needs_mmu registers the core's MMU on the attached
    prefetcher (BasePrefetcher.registerMMU). Prefetchers that train on
    virtual addresses (use_virtual_addresses=True, e.g. vimp) need this to
    translate page-crossing prefetch targets; without an MMU the queued
    prefetcher drops every candidate outside the trigger's page
    (Queued::insert in src/mem/cache/prefetch/queued.cc).
    """

    def __init__(
        self,
        l1d_size: str,
        l1i_size: str,
        l2_size: str,
        vector_l1d_size: str,
        vector_l2_size: str,
        membus=None,
        scalar_l1d_prefetcher=None,
        scalar_l2_prefetcher=None,
        vector_l1d_prefetcher=None,
        vector_l2_prefetcher=None,
        prefetcher_needs_mmu: bool = False,
        # MSHR counts bound the miss-level parallelism of each cache;
        # tgts_per_mshr bounds how many demands can coalesce on one
        # outstanding line (relevant for gathers, where many elements of
        # one vluxei hit the same missing line). Defaults match the
        # stdlib L1DCache/L2Cache classes.
        l1d_mshrs: int = 16,
        l2_mshrs: int = 20,
        vector_l1d_mshrs: int = 16,
        vector_l2_mshrs: int = 20,
        l1d_tgts_per_mshr: int = 20,
        l2_tgts_per_mshr: int = 12,
        vector_l1d_tgts_per_mshr: int = 20,
        vector_l2_tgts_per_mshr: int = 12,
    ) -> None:
        super().__init__(
            l1d_size=l1d_size,
            l1i_size=l1i_size,
            l2_size=l2_size,
            membus=membus,
        )
        self._vector_l1d_size = vector_l1d_size
        self._vector_l2_size = vector_l2_size
        self._l1d_mshrs = l1d_mshrs
        self._l2_mshrs = l2_mshrs
        self._vector_l1d_mshrs = vector_l1d_mshrs
        self._vector_l2_mshrs = vector_l2_mshrs
        self._l1d_tgts_per_mshr = l1d_tgts_per_mshr
        self._l2_tgts_per_mshr = l2_tgts_per_mshr
        self._vector_l1d_tgts_per_mshr = vector_l1d_tgts_per_mshr
        self._vector_l2_tgts_per_mshr = vector_l2_tgts_per_mshr
        self._scalar_l1d_prefetcher = scalar_l1d_prefetcher
        self._scalar_l2_prefetcher = scalar_l2_prefetcher
        self._vector_l1d_prefetcher = vector_l1d_prefetcher
        self._vector_l2_prefetcher = vector_l2_prefetcher
        self._prefetcher_needs_mmu = prefetcher_needs_mmu

    @overrides(AbstractCacheHierarchy)
    def incorporate_cache(self, board: AbstractBoard) -> None:
        # Same overall wiring as the parent class; the parent's body is
        # replicated here because the dcache connection in the middle of
        # its per-core loop has to go to the splitter instead.
        board.connect_system_port(self.membus.cpu_side_ports)

        for _, port in board.get_mem_ports():
            self.membus.mem_side_ports = port

        num_cores = board.get_processor().get_num_cores()
        self.l2buses = [L2XBar() for _ in range(num_cores)]
        self.vector_l2buses = [L2XBar() for _ in range(num_cores)]
        self.splitters = [VectorSplitter() for _ in range(num_cores)]

        for i, cpu in enumerate(board.get_processor().get_cores()):
            # Scalar chain, identical to PrivateL1PrivateL2CacheHierarchy.
            l2_node = self.add_root_child(
                f"l2-cache-{i}",
                L2Cache(
                    size=self._l2_size,
                    mshrs=self._l2_mshrs,
                    tgts_per_mshr=self._l2_tgts_per_mshr,
                ),
            )
            l1i_node = l2_node.add_child(
                f"l1i-cache-{i}", L1ICache(size=self._l1i_size)
            )
            l1d_node = l2_node.add_child(
                f"l1d-cache-{i}",
                L1DCache(
                    size=self._l1d_size,
                    mshrs=self._l1d_mshrs,
                    tgts_per_mshr=self._l1d_tgts_per_mshr,
                ),
            )

            # Baseline: disable prefetching on every scalar cache (the stdlib
            # L1I/L1D/L2 classes attach a StridePrefetcher by default), then
            # attach the selected prefetcher to a scalar cache if the caller
            # chose the scalar side. The vector chain below does the same for
            # the vector side, so at most one prefetcher is ever active.
            l2_node.cache.prefetcher = NULL
            l1i_node.cache.prefetcher = NULL
            l1d_node.cache.prefetcher = NULL

            # The factory is called once per core so each gets a fresh SimObject.
            if self._scalar_l2_prefetcher is not None:
                l2_node.cache.prefetcher = self._scalar_l2_prefetcher()
                if self._prefetcher_needs_mmu:
                    l2_node.cache.prefetcher.registerMMU(cpu.core.mmu)
            if self._scalar_l1d_prefetcher is not None:
                l1d_node.cache.prefetcher = self._scalar_l1d_prefetcher()
                if self._prefetcher_needs_mmu:
                    l1d_node.cache.prefetcher.registerMMU(cpu.core.mmu)

            self.l2buses[i].mem_side_ports = l2_node.cache.cpu_side
            self.membus.cpu_side_ports = l2_node.cache.mem_side

            l1i_node.cache.mem_side = self.l2buses[i].cpu_side_ports
            l1d_node.cache.mem_side = self.l2buses[i].cpu_side_ports

            # Vector chain: private L1D + L2, no L1I.
            vl2_node = self.add_root_child(
                f"vector-l2-cache-{i}",
                L2Cache(
                    size=self._vector_l2_size,
                    mshrs=self._vector_l2_mshrs,
                    tgts_per_mshr=self._vector_l2_tgts_per_mshr,
                ),
            )
            vl1d_node = vl2_node.add_child(
                f"vector-l1d-cache-{i}",
                L1DCache(
                    size=self._vector_l1d_size,
                    mshrs=self._vector_l1d_mshrs,
                    tgts_per_mshr=self._vector_l1d_tgts_per_mshr,
                ),
            )

            # Baseline: no prefetcher on either vector cache, overriding the
            # stdlib StridePrefetcher default.
            vl2_node.cache.prefetcher = NULL
            vl1d_node.cache.prefetcher = NULL

            # Attach the selected prefetcher to exactly one vector cache. The
            # factory is called once per core so each gets a fresh SimObject.
            if self._vector_l2_prefetcher is not None:
                vl2_node.cache.prefetcher = self._vector_l2_prefetcher()
                if self._prefetcher_needs_mmu:
                    vl2_node.cache.prefetcher.registerMMU(cpu.core.mmu)
            if self._vector_l1d_prefetcher is not None:
                vl1d_node.cache.prefetcher = self._vector_l1d_prefetcher()
                if self._prefetcher_needs_mmu:
                    vl1d_node.cache.prefetcher.registerMMU(cpu.core.mmu)

            self.vector_l2buses[i].mem_side_ports = vl2_node.cache.cpu_side
            self.membus.cpu_side_ports = vl2_node.cache.mem_side

            vl1d_node.cache.mem_side = self.vector_l2buses[i].cpu_side_ports

            # The splitter replaces the direct dcache->L1D connection.
            cpu.connect_icache(l1i_node.cache.cpu_side)
            cpu.connect_dcache(self.splitters[i].cpu_side_port)
            self.splitters[i].scalar_side_port = l1d_node.cache.cpu_side
            self.splitters[i].vector_side_port = vl1d_node.cache.cpu_side

            self._connect_table_walker(i, cpu)

            if board.get_processor().get_isa() == ISA.X86:
                int_req_port = self.membus.mem_side_ports
                int_resp_port = self.membus.cpu_side_ports
                cpu.connect_interrupt(int_req_port, int_resp_port)
            else:
                cpu.connect_interrupt()

        if board.has_coherent_io():
            self._setup_io_cache(board)
