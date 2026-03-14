# Copyright (c) 2025 Google LLC
# All rights reserved.
#
# Redistribution and use in source and binary forms, with or without
# modification, are permitted provided that the following conditions are
# met: redistributions of source code must retain the above copyright
# notice, this list of conditions and the following disclaimer;
# redistributions in binary form must reproduce the above copyright
# notice, this list of conditions and the following disclaimer in the
# documentation and/or other materials provided with the distribution;
# neither the name of the copyright holders nor the names of its
# contributors may be used to endorse or promote products derived from
# this software without specific prior written permission.
#
# THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
# "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
# LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
# A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
# OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
# SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
# LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
# DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
# THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
# (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
# OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

from m5.objects.FuncUnit import *
from m5.objects.FuncUnitConfig import *
from m5.objects import FUPool
from m5.params import *
from m5.SimObject import SimObject

class AraSIMD_Unit(FUDesc):
    """
    Custom Vector Functional Unit modeling the ARA processor pipeline.
    
    This class defines the specific latencies (opLat) for each vector operation class (opClass).
    The values are derived from the ARA hardware documentation specifications.
    
    Attributes:
        opList (list): A list of OpDesc objects, each mapping a specific instruction type (OpClass)
                       to a latency in cycles (opLat).
        count (int):   The number of identical units available in the CPU.
    """
    opList = [
        # --- Integer Arithmetic ---
        # ARA documentation specifies 1 cycle pipeline latency for basic integer ALU ops.
        OpDesc(opClass="SimdAdd", opLat=1),
        OpDesc(opClass="SimdAddAcc", opLat=1),
        OpDesc(opClass="SimdAlu", opLat=1),
        OpDesc(opClass="SimdCmp", opLat=1),
        OpDesc(opClass="SimdCvt", opLat=1),
        OpDesc(opClass="SimdMisc", opLat=1),
        OpDesc(opClass="SimdShift", opLat=1),
        OpDesc(opClass="SimdShiftAcc", opLat=1),
        
        # --- Integer Multiply ---
        # ARA implementation uses a pipelined multiplier.
        # Latency is effectively 1 cycle per element/instruction issue due to pipelining.
        OpDesc(opClass="SimdMult", opLat=1),
        OpDesc(opClass="SimdMultAcc", opLat=1),
        OpDesc(opClass="SimdMatMultAcc", opLat=1),
        
        # --- Integer Divide ---
        # ARA uses a serial divider with variable latency (up to 64 cycles).
        # We set a representative average latency of 32 cycles.
        # pipelined=False indicates the unit cannot accept new instructions until the current one finishes.
        OpDesc(opClass="SimdDiv", opLat=32, pipelined=False),
        
        # --- Float Arithmetic ---
        # ARA Floating Point Unit (FPU) latencies are higher than integer units.
        # We use the conservative maximum latency (for 64-bit elements) of 5 cycles.
        OpDesc(opClass="SimdFloatAdd", opLat=5),
        OpDesc(opClass="SimdFloatAlu", opLat=5), # Explicitly set to 5
        OpDesc(opClass="SimdFloatMult", opLat=5),
        OpDesc(opClass="SimdFloatMultAcc", opLat=5),
        OpDesc(opClass="SimdFloatMatMultAcc", opLat=5),
        
        # --- Float Misc / Compare ---
        # Comparisons are faster, typically 1 cycle.
        OpDesc(opClass="SimdFloatCmp", opLat=1),
        OpDesc(opClass="SimdFloatMisc", opLat=1),
        
        # --- Float Conversion ---
        # Floating point conversion operations take 2 cycles in ARA.
        OpDesc(opClass="SimdFloatCvt", opLat=2),
        
        # --- Float Divide / Square Root ---
        # Iterative operations with high latency.
        # Setting a conservative latency of 10 cycles. 
        # pipelined=False because the iterative unit is not fully pipelined in the same way.
        OpDesc(opClass="SimdFloatDiv", opLat=10, pipelined=False),
        OpDesc(opClass="SimdFloatSqrt", opLat=10, pipelined=False),
        
        # --- Reductions ---
        # Reduction operations effectively feed back into the pipeline.
        # Base latency is 1 cycle.
        OpDesc(opClass="SimdReduceAdd", opLat=1),
        OpDesc(opClass="SimdReduceAlu", opLat=1),
        OpDesc(opClass="SimdReduceCmp", opLat=1),
        OpDesc(opClass="SimdFloatReduceAdd", opLat=1),
        OpDesc(opClass="SimdFloatReduceCmp", opLat=1),
        
        # --- Load / Store Address Generation ---
        # These latencies represent the Address Generation Unit (AGU) time.
        # This is strictly the time to calculate addresses and issue requests to the memory system.
        # The actual memory access latency is modeled separately by the cache and memory controllers
        # connected to the CPU. 1 cycle is standard for AGU.
        OpDesc(opClass="SimdUnitStrideLoad", opLat=1),
        OpDesc(opClass="SimdUnitStrideStore", opLat=1),
        OpDesc(opClass="SimdUnitStrideMaskLoad", opLat=1),
        OpDesc(opClass="SimdUnitStrideMaskStore", opLat=1),
        OpDesc(opClass="SimdStridedLoad", opLat=1),
        OpDesc(opClass="SimdStridedStore", opLat=1),
        OpDesc(opClass="SimdIndexedLoad", opLat=1),
        OpDesc(opClass="SimdIndexedStore", opLat=1),
        OpDesc(opClass="SimdWholeRegisterLoad", opLat=1),
        OpDesc(opClass="SimdWholeRegisterStore", opLat=1),
        OpDesc(opClass="SimdUnitStrideSegmentedLoad", opLat=1),
        OpDesc(opClass="SimdUnitStrideSegmentedStore", opLat=1),
        OpDesc(opClass="SimdExt", opLat=1),
        OpDesc(opClass="SimdFloatExt", opLat=1),
        OpDesc(opClass="SimdConfig", opLat=1),
    ]
    
    # count=4 means the CPU effectively has 4 of these vector units available.
    # This models a superscalar capability where the CPU can issue up to 4 vector instructions
    # per cycle if dependencies allow, mimicking the high throughput of the ARA vector engine.
    count = 4

class AraFUPool(FUPool):
    """
    Custom Functional Unit Pool for an ARA-like O3 CPU configuration.
    
    This pool aggregates all functional units available to the CPU.
    It includes the standard scalar units (IntALU, FP_ALU, etc.) and replaces the 
    default vector unit with our custom 'AraSIMD_Unit'.
    """
    FUList = [
        IntALU(),       # Standard Integer ALUs (Scalar)
        IntMultDiv(),   # Standard Integer Multiply/Divide (Scalar)
        FP_ALU(),       # Standard Floating Point ALUs (Scalar)
        FP_MultDiv(),   # Standard Floating Point Mult/Div (Scalar)
        ReadPort(),     # Memory Read Ports
        AraSIMD_Unit(), # <--- Custom ARA Vector Unit defined above
        Matrix_Unit(),  # Matrix Unit (if used)
        System_Unit(),  # System instructions
        PredALU(),      # Predicated ALU
        WritePort(),    # Memory Write Ports
        RdWrPort(),     # Read/Write Ports
    ]

try:
    from m5.objects import RiscvO3CPU
    class AraO3CPU(RiscvO3CPU):
        """
        Custom RiscvO3CPU that automatically uses the AraFUPool.
        
        This class handles the boiler-plate of assigning the custom functional unit pool
        to both the Core (backend) and the Instruction Queues (IQ), which is required
        because the standard IQUnit defaults to a standard FUPool.
        """
        def __init__(self, **kwargs):
            super().__init__(**kwargs)
            
            # Create a dedicated ARA FUPool for the Core's Execute stage
            simd_unit = AraSIMD_Unit()
            simd_unit.count = self.simd_units

            self.fuPool = AraFUPool()
            # Replace the default AraSIMD_Unit in the pool with our custom-count one
            new_fu_list = []
            for fu in self.fuPool.FUList:
                if isinstance(fu, AraSIMD_Unit):
                    new_fu_list.append(simd_unit)
                else:
                    new_fu_list.append(fu)
            self.fuPool.FUList = new_fu_list
            
            # Assign to InstQueues (for Issue logic usage)
            # We MUST create unique instances for each IQ unit to avoid 
            # configuration hierarchy cycles and "orphan node" errors.
            for iq in self.instQueues:
                iq_simd_unit = AraSIMD_Unit()
                iq_simd_unit.count = self.simd_units
                
                iq_fu_pool = AraFUPool()
                iq_new_fu_list = []
                for fu in iq_fu_pool.FUList:
                    if isinstance(fu, AraSIMD_Unit):
                        iq_new_fu_list.append(iq_simd_unit)
                    else:
                        iq_new_fu_list.append(fu)
                iq_fu_pool.FUList = iq_new_fu_list
                iq.fuPool = iq_fu_pool

except ImportError:
    # RiscvO3CPU might not be available if not building for RISCV or if running 
    # check scripts. We pass to avoid breaking imports in those cases.
    pass
