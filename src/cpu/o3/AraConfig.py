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
    opList = [
        # Integer Arithmetic (1 cycle)
        OpDesc(opClass="SimdAdd", opLat=1),
        OpDesc(opClass="SimdAddAcc", opLat=1),
        OpDesc(opClass="SimdAlu", opLat=1),
        OpDesc(opClass="SimdCmp", opLat=1),
        OpDesc(opClass="SimdCvt", opLat=1),
        OpDesc(opClass="SimdMisc", opLat=1),
        OpDesc(opClass="SimdShift", opLat=1),
        OpDesc(opClass="SimdShiftAcc", opLat=1),
        
        # Integer Multiply (1 cycle pipeline)
        OpDesc(opClass="SimdMult", opLat=1),
        OpDesc(opClass="SimdMultAcc", opLat=1),
        OpDesc(opClass="SimdMatMultAcc", opLat=1),
        
        # Integer Divide (Variable, setting to a representative average or max)
        # ARA docs say serial divider, can be up to 64 cycles.
        # Setting to a conservative average for now.
        OpDesc(opClass="SimdDiv", opLat=32, pipelined=False),
        
        # Float Arithmetic (5 cycles for 64-bit, conservative)
        OpDesc(opClass="SimdFloatAdd", opLat=5),
        OpDesc(opClass="SimdFloatAlu", opLat=5),
        OpDesc(opClass="SimdFloatMult", opLat=5),
        OpDesc(opClass="SimdFloatMultAcc", opLat=5),
        OpDesc(opClass="SimdFloatMatMultAcc", opLat=5),
        
        # Float Misc / Comp (1 cycle)
        OpDesc(opClass="SimdFloatCmp", opLat=1),
        OpDesc(opClass="SimdFloatMisc", opLat=1),
        
        # Float Conversion (2 cycles)
        OpDesc(opClass="SimdFloatCvt", opLat=2),
        
        # Float Div/Sqrt (3 cycles base + iterative)
        # Setting to a conservative latency for iterative operations
        OpDesc(opClass="SimdFloatDiv", opLat=10, pipelined=False),
        OpDesc(opClass="SimdFloatSqrt", opLat=10, pipelined=False),
        
        # Reductions (1 cycle)
        OpDesc(opClass="SimdReduceAdd", opLat=1),
        OpDesc(opClass="SimdReduceAlu", opLat=1),
        OpDesc(opClass="SimdReduceCmp", opLat=1),
        OpDesc(opClass="SimdFloatReduceAdd", opLat=1),
        OpDesc(opClass="SimdFloatReduceCmp", opLat=1),
        
        # Load/Store Address Gen (1 cycle)
        # Note: Actual memory access time added by cache/memory system
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
    count = 4

class AraFUPool(FUPool):
    FUList = [
        IntALU(),
        IntMultDiv(),
        FP_ALU(),
        FP_MultDiv(),
        ReadPort(),
        AraSIMD_Unit(), # Custom ARA Vector Unit
        Matrix_Unit(),
        System_Unit(),
        PredALU(),
        WritePort(),
        RdWrPort(),
    ]
