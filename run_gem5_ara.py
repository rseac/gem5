import os
import sys
import m5
from m5.objects import *

if len(sys.argv) < 2:
    print("Usage: run_gem5_ara.py <benchmark.elf> [args...]")
    sys.exit(1)

binary = sys.argv[1]
binary_args = sys.argv[2:]
if len(binary_args) == 1 and ' ' in binary_args[0]:
    binary_args = binary_args[0].split()

system = System()
system.clk_domain = SrcClockDomain()
system.clk_domain.clock = "1GHz"
system.clk_domain.voltage_domain = VoltageDomain()

system.mem_mode = "timing"
system.mem_ranges = [AddrRange(start=0x80000000, size="2GB")]

# Tune MinorCPU to match CVA6 (single issue, in-order)
#
# The default FU pool has exactly one MinorDefaultMemFU() instance covering
# EVERY memory op class, so every memory micro-op - including each element
# of a strided/indexed vector load - contends for the same single
# functional unit. MinorCPU's FUs are self-stalling: once one non-bubble
# instruction sits at an FU's tail awaiting commit, that FU accepts no new
# instruction until it commits, which for a memory op means waiting for its
# response. A single-issue core can still have several *outstanding*
# memory requests via a non-blocking memory system - the real Ara RTL's own
# address generator supports VaddrgenInsnQueueDepth=4 outstanding
# per-element requests (see ara/hardware/include/ara_pkg.sv) - but that
# queue is specifically for non-unit-stride (strided/indexed) accesses,
# where each element's address is independently computed. Unit-stride
# accesses coalesce into a single wide burst in real hardware and don't
# need (or get) that same per-element outstanding-request depth.
#
# An earlier version of this fix widened ALL memory op classes to 4 FU
# instances uniformly. That over-corrected unit-stride-only benchmarks
# (e.g. axpy's MAPE got WORSE, 6.5% -> 37.6%, because it doesn't have the
# per-element contention this fix targets) while still helping genuinely
# strided/indexed-bound ones (matmul, spmv, streamcluster). Splitting the
# pool so only SimdStridedLoad/Store and SimdIndexedLoad/Store get the 4x
# concurrency - matched to Ara's real address-generation queue depth -
# while everything else (MemRead/MemWrite, unit-stride, whole-register,
# fault-only-first) keeps the original single-FU model, should recover
# axpy's earlier accuracy without losing the strided/indexed benchmarks'
# gains.
class UnitStrideMemFU(MinorFU):
    opClasses = minorMakeOpClassSet(
        [
            "MemRead",
            "MemWrite",
            "FloatMemRead",
            "FloatMemWrite",
            "SimdUnitStrideLoad",
            "SimdUnitStrideStore",
            "SimdUnitStrideMaskLoad",
            "SimdUnitStrideMaskStore",
            "SimdUnitStrideFaultOnlyFirstLoad",
            "SimdWholeRegisterLoad",
            "SimdWholeRegisterStore",
        ]
    )
    timings = [
        MinorFUTiming(
            description="Mem", srcRegsRelativeLats=[1], extraAssumedLat=2
        )
    ]
    opLat = 1


class StridedIndexedMemFU(MinorFU):
    opClasses = minorMakeOpClassSet(
        [
            "SimdStridedLoad",
            "SimdStridedStore",
            "SimdIndexedLoad",
            "SimdIndexedStore",
        ]
    )
    timings = [
        MinorFUTiming(
            description="Mem", srcRegsRelativeLats=[1], extraAssumedLat=2
        )
    ]
    opLat = 1


mem_fu_pool = MinorDefaultFUPool()
mem_fu_pool.funcUnits = [
    MinorDefaultIntFU(),
    MinorDefaultIntFU(),
    MinorDefaultIntMulFU(),
    MinorDefaultIntDivFU(),
    MinorDefaultFloatSimdFU(),
    MinorDefaultPredFU(),
    UnitStrideMemFU(),
    StridedIndexedMemFU(),
    StridedIndexedMemFU(),
    StridedIndexedMemFU(),
    StridedIndexedMemFU(),
    MinorDefaultMiscFU(),
]

system.cpu = MinorCPU(
    decodeInputWidth=1,
    executeInputWidth=1,
    executeIssueLimit=1,
    executeCommitLimit=1,
    fetch1ToFetch2ForwardDelay=1,
    fetch2ToDecodeForwardDelay=1,
    decodeToExecuteForwardDelay=2,
    executeBranchDelay=2,
    executeFuncUnits=mem_fu_pool,
    executeMaxAccessesInMemory=4,
)


# RiscvISA's own vlen (default 256 bits) is the *real* architectural VLEN
# that vsetvli honors - it is a separate parameter from AraCoprocessor's
# own vlen below (which only sizes the coprocessor's internal mock model)
# and was previously left unset, silently running every benchmark at 1/16
# of the RTL baseline's VLEN=4096 and thus ~16x more vector-loop trips.
system.cpu.isa = [RiscvISA(vlen=4096, elen=64)]

system.cpu.araCoprocessor = AraCoprocessor(num_lanes=4, vlen=4096)
system.membus = SystemXBar(width=512, forward_latency=0, response_latency=0, snoop_response_latency=0, header_latency=0)
system.cpu.icache_port = system.membus.cpu_side_ports
system.cpu.dcache_port = system.membus.cpu_side_ports
system.cpu.araCoprocessor.dcache_port = system.membus.cpu_side_ports
system.cpu.createInterruptController()

system.mem_ctrl = SimpleMemory(latency="0ns", bandwidth="64GB/s")
system.mem_ctrl.range = system.mem_ranges[0]
system.mem_ctrl.port = system.membus.mem_side_ports
system.system_port = system.membus.cpu_side_ports

system.workload = SEWorkload.init_compatible(binary)
process = Process()
process.cmd = [binary] + binary_args
print("process.cmd:", process.cmd)
system.cpu.workload = process
system.cpu.createThreads()

root = Root(full_system=False, system=system)
m5.instantiate()

# A normal glibc-linked SE-mode binary gets its stack/heap/BSS mapped
# automatically by Process/SEWorkload - no manual identity-mapping of
# bare-metal-testbench-style fixed addresses is needed here.

print("Beginning simulation for %s!" % binary)
exit_event = m5.simulate()
print("Exiting @ tick %i because %s" % (m5.curTick(), exit_event.getCause()))
