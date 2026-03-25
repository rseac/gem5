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
    We set opLat=6 to match the RTL 6-cycle dispatch floor for dependent chains.
    """
    opList = [
        OpDesc(opClass="SimdAdd", opLat=6),
        OpDesc(opClass="SimdAddAcc", opLat=6),
        OpDesc(opClass="SimdAlu", opLat=6),
        OpDesc(opClass="SimdCmp", opLat=6),
        OpDesc(opClass="SimdCvt", opLat=6),
        OpDesc(opClass="SimdMisc", opLat=6),
        OpDesc(opClass="SimdShift", opLat=6),
        OpDesc(opClass="SimdShiftAcc", opLat=6),
        OpDesc(opClass="SimdMult", opLat=6),
        OpDesc(opClass="SimdMultAcc", opLat=6),
        OpDesc(opClass="SimdMatMultAcc", opLat=6),
        OpDesc(opClass="SimdDiv", opLat=34, pipelined=False),
        OpDesc(opClass="SimdFloatAdd", opLat=6),
        OpDesc(opClass="SimdFloatAlu", opLat=6),
        OpDesc(opClass="SimdFloatMult", opLat=6),
        OpDesc(opClass="SimdFloatMultAcc", opLat=6),
        OpDesc(opClass="SimdFloatMatMultAcc", opLat=6),
        OpDesc(opClass="SimdFloatCmp", opLat=6),
        OpDesc(opClass="SimdFloatMisc", opLat=6),
        OpDesc(opClass="SimdFloatCvt", opLat=6),
        OpDesc(opClass="SimdFloatDiv", opLat=10, pipelined=False),
        OpDesc(opClass="SimdFloatSqrt", opLat=10, pipelined=False),
        OpDesc(opClass="SimdReduceAdd", opLat=6),
        OpDesc(opClass="SimdReduceAlu", opLat=6),
        OpDesc(opClass="SimdReduceCmp", opLat=6),
        OpDesc(opClass="SimdFloatReduceAdd", opLat=6),
        OpDesc(opClass="SimdFloatReduceCmp", opLat=6),
        OpDesc(opClass="SimdUnitStrideLoad", opLat=6),
        OpDesc(opClass="SimdUnitStrideStore", opLat=6),
        OpDesc(opClass="SimdUnitStrideMaskLoad", opLat=6),
        OpDesc(opClass="SimdUnitStrideMaskStore", opLat=6),
        OpDesc(opClass="SimdStridedLoad", opLat=6),
        OpDesc(opClass="SimdStridedStore", opLat=6),
        OpDesc(opClass="SimdIndexedLoad", opLat=6),
        OpDesc(opClass="SimdIndexedStore", opLat=6),
        OpDesc(opClass="SimdWholeRegisterLoad", opLat=6),
        OpDesc(opClass="SimdWholeRegisterStore", opLat=6),
        OpDesc(opClass="SimdUnitStrideSegmentedLoad", opLat=6),
        OpDesc(opClass="SimdUnitStrideSegmentedStore", opLat=6),
        OpDesc(opClass="SimdExt", opLat=6),
        OpDesc(opClass="SimdFloatExt", opLat=6),
        OpDesc(opClass="SimdConfig", opLat=6),
    ]
    count = 1

class RVVCore(BaseCPUCore):
    def __init__(self, elen, vlen, cpu_id, cpu_type, enable_chaining, vector_throughput, simd_units):
        if cpu_type == "AraO3":
            from cpu.o3.AraConfig import AraO3CPU as SelectedCPU
            # Stable Frontend (Defaults)
            core = SelectedCPU(cpu_id=cpu_id)
            
            # CRITICAL: Force Functional Unit Substitution
            calibrated_units = [CalibratedAraSIMD_Unit() for _ in range(simd_units)]
            for iq in core.instQueues:
                iq.fuPool = FUPool(FUList = [
                    IntALU(), IntMultDiv(), FP_ALU(), FP_MultDiv(),
                    ReadPort(), 
                    Matrix_Unit(), System_Unit(), PredALU(),
                    WritePort(), RdWrPort()
                ] + calibrated_units)
        else:
            from cpu.minor.AraMinorConfig import AraMinorCPU as SelectedCPU
            core = SelectedCPU(cpu_id=cpu_id)
            
        super().__init__(core=core, isa=ISA.RISCV)
        
        # --- DEEP INJECTION ---
        # We must set VLEN and Timing parameters directly on the ISA objects
        # This is where gem5 instructions actually look for these values.
        for isa in self.core.isa:
            isa.vlen = vlen
            isa.elen = elen
            isa.enable_vector_chaining = enable_chaining
            isa.vector_timing_throughput = vector_throughput

# --- CLI ---
parser = argparse.ArgumentParser()
parser.add_argument("resource", type=str)
parser.add_argument("-v", "--vlen", required=False, type=int, default=4096)
parser.add_argument("-e", "--elen", required=False, type=int, default=64)
parser.add_argument("-d", "--l1d", required=False, type=str, default="32KiB")
parser.add_argument("-2", "--l2", required=False, type=str, default="512KiB")
parser.add_argument("-p", "--parms", required=False, type=str, default='')
parser.add_argument("--cpu-type", type=str, default="AraO3")
parser.add_argument("--vector-timing-throughput", type=int, default=4)
parser.add_argument("--simd-units", type=int, default=2)
parser.add_argument("--enable-chaining", action="store_true", default=True)

args = parser.parse_args()

# --- System Construction ---
cache_hierarchy = PrivateL1PrivateL2CacheHierarchy(l1d_size=args.l1d, l1i_size="32KiB", l2_size=args.l2)
memory = SingleChannelDDR4_2400(size="8GiB")
processor = BaseCPUProcessor(
    cores=[RVVCore(args.elen, args.vlen, 0, args.cpu_type, 
                   True, args.vector_timing_throughput, 
                   args.simd_units)]
)

board = SimpleBoard(clk_freq="1GHz", processor=processor, memory=memory, cache_hierarchy=cache_hierarchy)
binary = res.BinaryResource(args.resource)
board.set_se_binary_workload(binary, arguments=args.parms.split())

print("\n" + "="*60)
print(f"   ARA CALIBRATED SIMULATION: VLEN={args.vlen}, LANES={args.simd_units}")
print(f"   Applying Deep ISA Injection for Timing & Throughput")
print("="*60 + "\n")

simulator = Simulator(board=board, full_system=False)
simulator.run()
