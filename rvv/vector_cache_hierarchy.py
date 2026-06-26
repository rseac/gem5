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
    each one to NULL), then the optional vector_l1d_prefetcher /
    vector_l2_prefetcher factory is attached to exactly one vector cache.
    The net effect is at most one active prefetcher, on the cache the caller
    selects, and none at all when no factory is passed.

    vector_l1d_prefetcher / vector_l2_prefetcher are each a zero-arg factory
    (callable -> a fresh prefetcher SimObject); the factory is invoked once
    per core because a SimObject cannot be shared between caches. See
    rvv/prefetcher_factory.py.
    """

    def __init__(
        self,
        l1d_size: str,
        l1i_size: str,
        l2_size: str,
        vector_l1d_size: str,
        vector_l2_size: str,
        membus=None,
        vector_l1d_prefetcher=None,
        vector_l2_prefetcher=None,
    ) -> None:
        super().__init__(
            l1d_size=l1d_size,
            l1i_size=l1i_size,
            l2_size=l2_size,
            membus=membus,
        )
        self._vector_l1d_size = vector_l1d_size
        self._vector_l2_size = vector_l2_size
        self._vector_l1d_prefetcher = vector_l1d_prefetcher
        self._vector_l2_prefetcher = vector_l2_prefetcher

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
                f"l2-cache-{i}", L2Cache(size=self._l2_size)
            )
            l1i_node = l2_node.add_child(
                f"l1i-cache-{i}", L1ICache(size=self._l1i_size)
            )
            l1d_node = l2_node.add_child(
                f"l1d-cache-{i}", L1DCache(size=self._l1d_size)
            )

            # Disable prefetching on every scalar cache (the stdlib L1I/L1D/L2
            # classes attach a StridePrefetcher by default). The vector chain
            # below sets its own baseline and attaches the selected prefetcher,
            # so at most one prefetcher is ever active.
            l2_node.cache.prefetcher = NULL
            l1i_node.cache.prefetcher = NULL
            l1d_node.cache.prefetcher = NULL

            self.l2buses[i].mem_side_ports = l2_node.cache.cpu_side
            self.membus.cpu_side_ports = l2_node.cache.mem_side

            l1i_node.cache.mem_side = self.l2buses[i].cpu_side_ports
            l1d_node.cache.mem_side = self.l2buses[i].cpu_side_ports

            # Vector chain: private L1D + L2, no L1I.
            vl2_node = self.add_root_child(
                f"vector-l2-cache-{i}", L2Cache(size=self._vector_l2_size)
            )
            vl1d_node = vl2_node.add_child(
                f"vector-l1d-cache-{i}",
                L1DCache(size=self._vector_l1d_size),
            )

            # Baseline: no prefetcher on either vector cache, overriding the
            # stdlib StridePrefetcher default.
            vl2_node.cache.prefetcher = NULL
            vl1d_node.cache.prefetcher = NULL

            # Attach the selected prefetcher to exactly one vector cache. The
            # factory is called once per core so each gets a fresh SimObject.
            if self._vector_l2_prefetcher is not None:
                vl2_node.cache.prefetcher = self._vector_l2_prefetcher()
            if self._vector_l1d_prefetcher is not None:
                vl1d_node.cache.prefetcher = self._vector_l1d_prefetcher()

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
