# Copyright (c) 2024 Barcelona Supercomputing Center
# (Standard License Header ...)

import argparse
import sys
import os

sys.path.append(os.path.join(os.path.dirname(os.path.abspath(__file__)), '../src'))
sys.path.append(os.path.join(os.path.dirname(os.path.abspath(__file__)), '/gem5/src'))

from gem5.components.boards.simple_board import SimpleBoard
from gem5.components.cachehierarchies.classic.private_l1_private_l2_cache_hierarchy import (
    PrivateL1PrivateL2CacheHierarchy,
)
from gem5.components.memory import SingleChannelDDR4_2400
from gem5.components.processors.base_cpu_core import BaseCPUCore
from gem5.components.processors.base_cpu_processor import BaseCPUProcessor
from gem5.isas import ISA
import gem5.resources.resource as res
from gem5.simulate.simulator import Simulator
from gem5.utils.requires import requires

from m5.objects.FuncUnit import OpDesc, FUDesc
from m5.objects.FUPool import FUPool
from m5.objects.FuncUnitConfig import *

requires(isa_required=ISA.RISCV)

# --- Calibrated Functional Unit Definition ---
class CalibratedAraSIMD_Unit(FUDesc):
    """
    Calibrated ARA SIMD Unit.
    opLat=3: 1-cycle pipe + 2-cycle sync overhead.
    issueLat=6: CVA6 dispatch bottleneck.
    """
    opList = [
        OpDesc(opClass="SimdAdd", opLat=3, issueLat=6),
        OpDesc(opClass="SimdAddAcc", opLat=3, issueLat=6),
        OpDesc(opClass="SimdAlu", opLat=3, issueLat=6),
        OpDesc(opClass="SimdCmp", opLat=3, issueLat=6),
        OpDesc(opClass="SimdCvt", opLat=3, issueLat=6),
        OpDesc(opClass="SimdMisc", opLat=3, issueLat=6),
        OpDesc(opClass="SimdShift", opLat=3, issueLat=6),
        OpDesc(opClass="SimdShiftAcc", opLat=3, issueLat=6),
        OpDesc(opClass="SimdMult", opLat=3, issueLat=6),
        OpDesc(opClass="SimdMultAcc", opLat=3, issueLat=6),
        OpDesc(opClass="SimdMatMultAcc", opLat=3, issueLat=6),
        OpDesc(opClass="SimdDiv", opLat=34, issueLat=6, pipelined=False),
        OpDesc(opClass="SimdFloatAdd", opLat=6, issueLat=6),
        OpDesc(opClass="SimdFloatAlu", opLat=6, issueLat=6),
        OpDesc(opClass="SimdFloatMult", opLat=6, issueLat=6),
        OpDesc(opClass="SimdFloatMultAcc", opLat=6, issueLat=6),
        OpDesc(opClass="SimdFloatMatMultAcc", opLat=6, issueLat=6),
        OpDesc(opClass="SimdFloatCmp", opLat=3, issueLat=6),
        OpDesc(opClass="SimdFloatMisc", opLat=3, issueLat=6),
        OpDesc(opClass="SimdFloatCvt", opLat=4, issueLat=6),
        OpDesc(opClass="SimdFloatDiv", opLat=5, issueLat=6, pipelined=False),
        OpDesc(opClass="SimdFloatSqrt", opLat=5, issueLat=6, pipelined=False),
        OpDesc(opClass="SimdReduceAdd", opLat=3, issueLat=6),
        OpDesc(opClass="SimdReduceAlu", opLat=3, issueLat=6),
        OpDesc(opClass="SimdReduceCmp", opLat=3, issueLat=6),
        OpDesc(opClass="SimdFloatReduceAdd", opLat=3, issueLat=6),
        OpDesc(opClass="SimdFloatReduceCmp", opLat=3, issueLat=6),
        OpDesc(opClass="SimdUnitStrideLoad", opLat=3, issueLat=6),
        OpDesc(opClass="SimdUnitStrideStore", opLat=3, issueLat=6),
        OpDesc(opClass="SimdUnitStrideMaskLoad", opLat=3, issueLat=6),
        OpDesc(opClass="SimdUnitStrideMaskStore", opLat=3, issueLat=6),
        OpDesc(opClass="SimdStridedLoad", opLat=3, issueLat=6),
        OpDesc(opClass="SimdStridedStore", opLat=3, issueLat=6),
        OpDesc(opClass="SimdIndexedLoad", opLat=3, issueLat=6),
        OpDesc(opClass="SimdIndexedStore", opLat=3, issueLat=6),
        OpDesc(opClass="SimdWholeRegisterLoad", opLat=3, issueLat=6),
        OpDesc(opClass="SimdWholeRegisterStore", opLat=3, issueLat=6),
        OpDesc(opClass="SimdUnitStrideSegmentedLoad", opLat=3, issueLat=6),
        OpDesc(opClass="SimdUnitStrideSegmentedStore", opLat=3, issueLat=6),
        OpDesc(opClass="SimdExt", opLat=3, issueLat=6),
        OpDesc(opClass="SimdFloatExt", opLat=3, issueLat=6),
        OpDesc(opClass="SimdConfig", opLat=3, issueLat=6),
    ]
    count = 4

class RVVCore(BaseCPUCore):
    def __init__(self, elen, vlen, cpu_id, cpu_type, enable_chaining, vector_throughput, simd_units):
        if cpu_type == "AraO3":
            from cpu.o3.AraConfig import AraO3CPU as SelectedCPU
            # Create core with Lean Frontend
            core = SelectedCPU(cpu_id=cpu_id,
                            fetchWidth=1, decodeWidth=1, renameWidth=1,
                            dispatchWidth=1, issueWidth=1, wbWidth=1,
                            commitWidth=1, squashWidth=1,
                            fetchToDecodeDelay=2, decodeToRenameDelay=2,
                            renameToIEWDelay=2, renameToROBDelay=2,
                            iewToCommitDelay=2, iewToRenameDelay=2)
            
            # Substitute the Functional Unit Pool with our calibrated version
            for iq in core.instQueues:
                iq.fuPool = FUPool(FUList = [
                    IntALU(), IntMultDiv(), FP_ALU(), FP_MultDiv(),
                    ReadPort(), CalibratedAraSIMD_Unit(count=simd_units),
                    Matrix_Unit(), System_Unit(), PredALU(),
                    WritePort(), RdWrPort()
                ])
        else:
            from cpu.minor.AraMinorConfig import AraMinorCPU as SelectedCPU
            core = SelectedCPU(cpu_id=cpu_id)
            
        core.enable_vector_chaining = enable_chaining
        core.vector_timing_throughput = vector_throughput
        core.simd_units = simd_units
        super().__init__(core=core, isa=ISA.RISCV)
        self.core.isa[0].elen = elen
        self.core.isa[0].vlen = vlen

# --- CLI & Setup ---
parser = argparse.ArgumentParser()
parser.add_argument("resource", type=str)
parser.add_argument("-v", "--vlen", required=False, type=int, default=4096)
parser.add_argument("-e", "--elen", required=False, type=int, default=64)
parser.add_argument("-d", "--l1d", required=False, type=str, default="32KiB")
parser.add_argument("-2", "--l2", required=False, type=str, default="512KiB")
parser.add_argument("-p", "--parms", required=False, type=str, default='')
parser.add_argument("--cpu-type", type=str, default="AraO3", choices=["AraO3", "AraMinor"])
parser.add_argument("--enable-chaining", action="store_true", default=True)
parser.add_argument("--vector-timing-throughput", type=int, default=4)
parser.add_argument("--simd-units", type=int, default=2)

args = parser.parse_args()

cache_hierarchy = PrivateL1PrivateL2CacheHierarchy(
    l1d_size=args.l1d, l1i_size="32KiB", l2_size=args.l2
)
memory = SingleChannelDDR4_2400(size="8GiB")
processor = BaseCPUProcessor(
    cores=[RVVCore(args.elen, args.vlen, 0, args.cpu_type, 
                   args.enable_chaining, args.vector_timing_throughput, 
                   args.simd_units)]
)

board = SimpleBoard(clk_freq="1GHz", processor=processor, memory=memory, cache_hierarchy=cache_hierarchy)
binary = res.BinaryResource(args.resource)
board.set_se_binary_workload(binary, arguments=args.parms.split())

print("\n" + "="*60)
print("   ARA HARDWARE-CALIBRATED SIMULATION ACTIVE")
print("   Config: Single-Issue Frontend, IssueLat=6, OpLat=3")
print("="*60 + "\n")

simulator = Simulator(board=board, full_system=False)
simulator.run()
