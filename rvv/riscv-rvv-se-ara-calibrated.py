# Copyright (c) 2024 Barcelona Supercomputing Center
# (Standard License Header ...)

import argparse
import sys
import os

sys.path.append(os.path.join(os.path.dirname(os.path.abspath(__file__)), '../src'))
sys.path.append(os.path.join(os.path.dirname(os.path.abspath(__file__)), '/gem5/src'))

# --- MONKEY PATCHING (Force Global Latencies) ---
import m5.objects.FuncUnitConfig as FUC
from m5.objects.FuncUnit import OpDesc

print("DEBUG: Applying Global Hardware Calibration...")
# Find every OpDesc that belongs to a Simd class and force 6-cycle latency
for attr_name in dir(FUC):
    attr = getattr(FUC, attr_name)
    if isinstance(attr, OpDesc) and attr.opClass.name.startswith('Simd'):
        attr.opLat = 6
        print(f"  -> Calibrated {attr_name} ({attr.opClass.name}) to 6 cycles")

# --- Standard Imports ---
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

requires(isa_required=ISA.RISCV)

class RVVCore(BaseCPUCore):
    def __init__(self, elen, vlen, cpu_id, cpu_type, enable_chaining, vector_throughput, simd_units):
        if cpu_type == "AraO3":
            from cpu.o3.AraConfig import AraO3CPU as SelectedCPU
            core = SelectedCPU(cpu_id=cpu_id,
                            fetchWidth=1, decodeWidth=1, renameWidth=1,
                            dispatchWidth=1, issueWidth=1, wbWidth=1,
                            commitWidth=1, squashWidth=1)
        else:
            from cpu.minor.AraMinorConfig import AraMinorCPU as SelectedCPU
            core = SelectedCPU(cpu_id=cpu_id)
            
        super().__init__(core=core, isa=ISA.RISCV)
        
        # --- ISA INJECTION ---
        param_map = {
            'vlen': vlen, 'elen': elen,
            'enable_chaining': enable_chaining,
            'enable_vector_chaining': enable_chaining,
            'vector_timing_throughput': vector_throughput,
            'timing_vector_throughput': vector_throughput
        }
        for isa in self.core.isa:
            for p_name, p_val in param_map.items():
                if hasattr(isa, p_name):
                    setattr(isa, p_name, p_val)

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

# --- Construction ---
cache_hierarchy = PrivateL1PrivateL2CacheHierarchy(l1d_size=args.l1d, l1i_size="32KiB", l2_size=args.l2)
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
print(f"   ARA CALIBRATED SIMULATION ACTIVE")
print(f"   VLEN={args.vlen}, Lanes={args.simd_units}, IssueFloor=6")
print("="*60 + "\n")

simulator = Simulator(board=board, full_system=False)
simulator.run()
