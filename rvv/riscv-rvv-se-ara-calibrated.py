# Copyright (c) 2024 Barcelona Supercomputing Center
# (Standard License Header ...)

"""
Calibrated ARA gem5 run script.
This version accurately models the CVA6 dispatch bottleneck (6 cycles)
and the single-issue frontend of the ARA hardware.
"""

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
from gem5.resources.resource import obtain_resource
from gem5.simulate.simulator import Simulator
from gem5.utils.requires import requires

requires(isa_required=ISA.RISCV)

class RVVCore(BaseCPUCore):
    def __init__(self, elen, vlen, cpu_id, enable_chaining, vector_timing_throughput, simd_units):
        # Import the selected CPU model
        from cpu.o3.AraConfig import AraO3CPU
        
        # Instantiate the core with Single-Issue Frontend (Calibrated for CVA6)
        core = AraO3CPU(cpu_id=cpu_id,
                        fetchWidth=1, decodeWidth=1, renameWidth=1,
                        dispatchWidth=1, issueWidth=1, wbWidth=1,
                        commitWidth=1, squashWidth=1,
                        # Stabilized delays to prevent TimeBuffer assertions
                        fetchToDecodeDelay=2, decodeToRenameDelay=2,
                        renameToIEWDelay=2, renameToROBDelay=2,
                        iewToCommitDelay=2, iewToRenameDelay=2)
            
        core.enable_vector_chaining = enable_chaining
        core.vector_timing_throughput = vector_timing_throughput
        core.simd_units = simd_units
        
        # --- Runtime Calibration of Functional Units ---
        # Force the 6-cycle issue bottleneck and 3-cycle pipe depth
        if hasattr(core, 'instQueues'):
            for iq in core.instQueues:
                if hasattr(iq, 'fuPool'):
                    for fu in iq.fuPool.FUList:
                        for op in fu.opList:
                            if op.opClass.startswith('Simd'):
                                op.issueLat = 6
                                if "Div" in op.opClass or "Sqrt" in op.opClass:
                                    op.opLat = op.opLat + 2
                                else:
                                    op.opLat = 3

        super().__init__(core=core, isa=ISA.RISCV)
        self.core.isa[0].elen = elen
        self.core.isa[0].vlen = vlen

# --- CLI Arguments ---
parser = argparse.ArgumentParser()
parser.add_argument("resource", type=str)
parser.add_argument("-v", "--vlen", required=False, type=int, default=4096)
parser.add_argument("-e", "--elen", required=False, type=int, default=64)
parser.add_argument("-d", "--l1d", required=False, type=str, default="32KiB")
parser.add_argument("-2", "--l2", required=False, type=str, default="512KiB")
parser.add_argument("-p", "--parms", required=False, type=str, default='')
parser.add_argument("--vector-timing-throughput", type=int, default=4)
parser.add_argument("--simd-units", type=int, default=2)

args = parser.parse_args()

# --- System Setup ---
cache_hierarchy = PrivateL1PrivateL2CacheHierarchy(
    l1d_size=args.l1d, l1i_size="32KiB", l2_size=args.l2
)
memory = SingleChannelDDR4_2400(size="8GiB")
processor = BaseCPUProcessor(
    cores=[RVVCore(args.elen, args.vlen, 0, True, 
                   args.vector_timing_throughput, args.simd_units)]
)

board = SimpleBoard(
    clk_freq="1GHz",
    processor=processor,
    memory=memory,
    cache_hierarchy=cache_hierarchy,
)

binary = res.BinaryResource(args.resource)
board.set_se_binary_workload(binary, arguments=args.parms.split())

# --- Execution ---
print("\n" + "="*60)
print("   ARA HARDWARE-CALIBRATED SIMULATION ACTIVE")
print("   Config: Single-Issue Frontend, 6-Cycle Dispatch Floor")
print("="*60 + "\n")

import m5
simulator = Simulator(board=board, full_system=False)
simulator.run()

cycles = int(m5.curTick() / 1000)
print(f"\nFinal Execution Cycles: {cycles}\n")
