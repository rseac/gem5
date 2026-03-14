# Copyright (c) 2024 Barcelona Supercomputing Center
#
# Redistribution and use in source and binary forms, with or without
# modification, are permitted provided that the following conditions are met:
#
# 1. Redistributions of source code must retain the above copyright notice, this
# list of conditions and the following disclaimer.
#
# 2. Redistributions in binary form must reproduce the above copyright notice,
# this list of conditions and the following disclaimer in the documentation
# and/or other materials provided with the distribution.
#
# 3. Neither the name of the copyright holder nor the names of its contributors
# may be used to endorse or promote products derived from this software without
# specific prior written permission.
#
# THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
# AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
# IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
# DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE
# FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
# DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
# SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
# CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
# OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
# OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

"""
This script demonstrates how to run RISC-V vector-enabled binaries in SE mode
with gem5. It accepts the number of CORES, VLEN, and ELEN as optional
parameters, as well as the resource name to run. If no resource name is
provided, a list of available resources will be displayed. If one is given the
simulation will then execute the specified resource binary with the selected
parameters until completion.

The riscv-rvv-example script has been modified to accept arbitrary binaries as resources, and
to allow configuration of L1d and L2 sizes in addition to CORES VLEN and DLEN
It's called riscv-rvv-se.py

Usage
-----

# Compile gem5 for RISC-V
scons build/RISCV/gem5.opt

# Run the simulation
./build/RISCV/gem5.opt riscv-rvv-se.py \
    [-c CORES] [-v VLEN] [-e ELEN] [-l L1D-size] [-2 L2-size] [-p [params to simulated program] <resource, i.e. programto simulate>

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
#from gem5.components.memory import SingleChannelDDR3_1600
from gem5.components.memory import SingleChannelDDR4_2400
from gem5.components.processors.base_cpu_core import BaseCPUCore
from gem5.components.processors.base_cpu_processor import BaseCPUProcessor
from gem5.isas import ISA
import gem5.resources.resource as res
from gem5.resources.resource import obtain_resource
from gem5.simulate.simulator import Simulator
from gem5.utils.requires import requires
# from cpu.o3.AraConfig import AraFUPool # Removed

class RVVCore(BaseCPUCore):
    def __init__(self, elen, vlen, cpu_id, enable_chaining, vector_timing_throughput, simd_units):
        # Use our custom SelectedCPU which handles FUPool configuration automatically
        core = SelectedCPU(cpu_id=cpu_id)
        core.enable_vector_chaining = enable_chaining
        core.vector_timing_throughput = vector_timing_throughput
        core.simd_units = simd_units
        super().__init__(core=core, isa=ISA.RISCV)
        self.core.isa[0].elen = elen
        self.core.isa[0].vlen = vlen



requires(isa_required=ISA.RISCV)

resources = [
    "rvv-branch",
    "rvv-index",
    "rvv-matmul",
    "rvv-memcpy",
    "rvv-reduce",
    "rvv-saxpy",
    "rvv-sgemm",
    "rvv-strcmp",
    "rvv-strcpy",
    "rvv-strlen",
    "rvv-strlen-fault",
    "rvv-strncpy",
]

parser = argparse.ArgumentParser()
parser.add_argument("resource", type=str)#, choices=resources)
parser.add_argument("-c", "--cores", required=False, type=int, default=1)
parser.add_argument("-v", "--vlen", required=False, type=int, default=256)
parser.add_argument("-e", "--elen", required=False, type=int, default=64) # spec only allows 64 bit elen
parser.add_argument("-d", "--l1d", required=False, type=str, default="32KiB")
parser.add_argument("-2", "--l2", required=False, type=str, default="512KiB")

parser.add_argument("-p", "--parms", required=False, type = str, default='2048')
parser.add_argument("--cpu-type", type=str, default="AraO3", choices=["AraO3", "AraMinor"], 
                    help="CPU model to use: AraO3 (O3CPU) or AraMinor (MinorCPU)")
parser.add_argument("--enable-chaining", action="store_true", default=True, help="Enable vector chaining")
parser.add_argument("--disable-chaining", action="store_false", dest="enable_chaining", help="Disable vector chaining")
parser.add_argument("--vector-timing-throughput", type=int, default=4, help="Number of elements per cycle for timing model")
parser.add_argument("--simd-units", type=int, default=2, help="Number of physical SIMD lanes")

args = parser.parse_args()

# Import the selected CPU model
if args.cpu_type == "AraO3":
    from cpu.o3.AraConfig import AraO3CPU as SelectedCPU
elif args.cpu_type == "AraMinor":
    from cpu.minor.AraMinorConfig import AraMinorCPU as SelectedCPU
else:
    print(f"Error: Unknown CPU type {args.cpu_type}")
    sys.exit(1)

cache_hierarchy = PrivateL1PrivateL2CacheHierarchy(
    #l1d_size="32KiB", l1i_size="32KiB", l2_size="512KiB"
    l1d_size=args.l1d, l1i_size="32KiB", l2_size=args.l2
)

#memory = SingleChannelDDR3_1600()
memory = SingleChannelDDR4_2400(size="8GiB")

processor = BaseCPUProcessor(
    cores=[RVVCore(args.elen, args.vlen, i, args.enable_chaining, args.vector_timing_throughput, args.simd_units) for i in range(args.cores)]
)

# --- VITAL: ASSIGN ARA FU POOL ---
# processor.fuPool = AraFUPool() # <--- Incorrect, ignored by BaseCPUProcessor
# ---------------------------------

board = SimpleBoard(
    clk_freq="1GHz",
    processor=processor,
    memory=memory,
    cache_hierarchy=cache_hierarchy,
)
if args.resource in resources:
    binary = obtain_resource(args.resource)
else:
    binary = res.BinaryResource(args.resource)

# --- Formatted Parameter Summary ---
print("=" * 50)
print("       ARA RISC-V VECTOR SIMUATION CONFIG")
print("-" * 50)
print(f"  Binary Resource:  {args.resource}")
print(f"  Program Args:     {args.parms}")
print(f"  CPU Model:        {args.cpu_type}")
print(f"  Cores:            {args.cores}")
print(f"  VLEN:             {args.vlen} bits")
print(f"  ELEN:             {args.elen} bits")
print(f"  Vector Chaining:  {'ENABLED' if args.enable_chaining else 'DISABLED'}")
print(f"  Throughput:       {args.vector_timing_throughput} elements/cycle")
print(f"  SIMD Units:       {args.simd_units} parallel units")
print(f"  L1D Cache:        {args.l1d}")
print(f"  L2 Cache:         {args.l2}")
print("-" * 50)
print("Beginning simulation...")
print("=" * 50)

# board.set_se_binary_workload(binary, arguments=[args.parms])
board.set_se_binary_workload(binary, arguments=args.parms.split())


simulator.run()

# Output cycles (assuming 1GHz clock as configured in SimpleBoard)
# 1GHz = 1000 ps period (default gem5 tick is 1ps)
cycles = m5.curTick() / 1000
print(f"Total Execution Cycles: {int(cycles)}")
