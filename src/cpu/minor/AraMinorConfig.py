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

import math
from m5.objects.BaseMinorCPU import *
from m5.objects.FuncUnit import *
from cpu.o3.AraConfig import AraLatencyModel

# Helper to create OpClassSets
def makeOpClassSet(op_classes):
    return MinorOpClassSet(opClasses=[MinorOpClass(opClass=o) for o in op_classes])

# --- Custom ARA Functional Units for MinorCPU ---

class AraMinorIntVectorFU(MinorFU):
    """Integer Vector Operations (Add, Mult, Misc) - 1 Cycle Latency"""
    def __init__(self, op_lat=1, **kwargs):
        super().__init__(**kwargs)
        self.opClasses = makeOpClassSet([
            "SimdAdd", "SimdAddAcc", "SimdAlu", "SimdCmp", "SimdCvt", 
            "SimdMisc", "SimdShift", "SimdShiftAcc",
            "SimdMult", "SimdMultAcc", "SimdMatMultAcc",
            "SimdReduceAdd", "SimdReduceAlu", "SimdReduceCmp"
        ])
        self.opLat = op_lat
        self.issueLat = 1

class AraMinorIntDivVectorFU(MinorFU):
    """Integer Vector Divide - ~32 Cycles Latency"""
    opClasses = makeOpClassSet(["SimdDiv"])
    opLat = 32
    issueLat = 1 # Pipelined to allow chaining start

class AraMinorFloatVectorFU(MinorFU):
    """Floating Point Vector Operations (Add, Mult, Mac) - 5 Cycles Latency"""
    def __init__(self, op_lat=5, **kwargs):
        super().__init__(**kwargs)
        self.opClasses = makeOpClassSet([
            "SimdFloatAdd", 
            "SimdFloatAlu", 
            "SimdFloatMult", "SimdFloatMultAcc", "SimdFloatMatMultAcc",
            "SimdFloatReduceAdd", "SimdFloatReduceCmp"
        ])
        self.opLat = op_lat
        self.issueLat = 1

class AraMinorFloatCmpFU(MinorFU):
    """Floating Point Compare/Misc - 1 Cycle Latency"""
    opClasses = makeOpClassSet(["SimdFloatCmp", "SimdFloatMisc"])
    opLat = 1
    issueLat = 1

class AraMinorFloatCvtFU(MinorFU):
    """Floating Point Conversion - 2 Cycles Latency"""
    opClasses = makeOpClassSet(["SimdFloatCvt"])
    opLat = 2
    issueLat = 1

class AraMinorFloatDivSqrtFU(MinorFU):
    """Floating Point Divide/Sqrt - ~10 Cycles Latency"""
    opClasses = makeOpClassSet(["SimdFloatDiv", "SimdFloatSqrt"])
    opLat = 10
    issueLat = 1 # Pipelined to allow chaining start

class AraMinorMemFU(MinorFU):
    """Vector Memory Operations (AGU) - 1 Cycle Latency (Ara) or 3 (AraXL)"""
    def __init__(self, op_lat=1, **kwargs):
        super().__init__(**kwargs)
        self.opClasses = makeOpClassSet([
            "SimdUnitStrideLoad", "SimdUnitStrideStore",
            "SimdUnitStrideMaskLoad", "SimdUnitStrideMaskStore",
            "SimdStridedLoad", "SimdStridedStore",
            "SimdIndexedLoad", "SimdIndexedStore",
            "SimdUnitStrideSegmentedLoad", "SimdUnitStrideSegmentedStore",
            "SimdWholeRegisterLoad", "SimdWholeRegisterStore",
            "SimdUnitStrideFaultOnlyFirstLoad", "SimdUnitStrideSegmentedFaultOnlyFirstLoad",
            "SimdExt", "SimdFloatExt", "SimdConfig"
        ])
        self.opLat = op_lat
        self.issueLat = 1

# --- Standard Functional Units (Reused/Modified) ---

class AraMinorIntFU(MinorDefaultIntFU):
    pass

class AraMinorIntMulFU(MinorDefaultIntMulFU):
    pass

class AraMinorIntDivFU(MinorDefaultIntDivFU):
    pass

class AraMinorFloatFU(MinorFU):
    """Standard Scalar Float Unit"""
    opClasses = makeOpClassSet([
        "FloatAdd", "FloatCmp", "FloatCvt", "FloatMisc", 
        "FloatMult", "FloatMultAcc", "FloatDiv", "FloatSqrt", "Bf16Cvt"
    ])
    opLat = 3 # Average latency for scalar float

class AraMinorMemStandardFU(MinorDefaultMemFU):
    """Standard Memory Unit (Scalar)"""
    pass

class AraMinorMiscFU(MinorDefaultMiscFU):
    pass

# --- The Pool ---

class AraMinorFUPool(MinorFUPool):
    """Custom Functional Unit Pool for ARA on MinorCPU"""
    def __init__(self, simd_units=2, is_araxl=False, nr_clusters=1,
                 ring_latency=2, **kwargs):
        super().__init__(**kwargs)
        
        # Calculate latencies
        mem_lat = 3 if is_araxl else 1
        
        inter_cluster_overhead = 0
        if is_araxl and nr_clusters > 1:
            inter_cluster_overhead = 2 * ring_latency * math.ceil(math.log2(nr_clusters))
        
        self.funcUnits = [
            AraMinorIntFU(),
            AraMinorIntFU(),
            AraMinorIntMulFU(),
            AraMinorIntDivFU(),
            AraMinorFloatFU(),
            AraMinorMiscFU(),
            AraMinorMemStandardFU(), # Handle scalar mem
            
            # ARA Vector Units (Primary)
            AraMinorIntVectorFU(op_lat=1 + inter_cluster_overhead),
            AraMinorIntDivVectorFU(),
            AraMinorFloatVectorFU(op_lat=5 + inter_cluster_overhead),
            AraMinorFloatCmpFU(),
            AraMinorFloatCvtFU(),
            AraMinorFloatDivSqrtFU(),
            AraMinorMemFU(op_lat=mem_lat) # Handle vector mem
        ]

        # Add extra sets of vector functional units if requested
        for i in range(simd_units - 1):
            self.funcUnits.extend([
                AraMinorIntVectorFU(op_lat=1 + inter_cluster_overhead),
                AraMinorIntDivVectorFU(),
                AraMinorFloatVectorFU(op_lat=5 + inter_cluster_overhead),
                AraMinorFloatCmpFU(),
                AraMinorFloatCvtFU(),
                AraMinorFloatDivSqrtFU(),
                AraMinorMemFU(op_lat=mem_lat)
            ])

# --- The CPU Wrapper ---

try:
    from m5.objects.RiscvCPU import RiscvMinorCPU
    
    class AraMinorCPU(RiscvMinorCPU):
        """
        Custom RiscvMinorCPU that automatically uses the AraMinorFUPool.
        """
        def __init__(self, simd_units=2, nr_clusters=1, is_araxl=False,
                     ring_latency=2, **kwargs):
            super().__init__(**kwargs)
            
            self.executeFuncUnits = AraMinorFUPool(
                simd_units=simd_units,
                is_araxl=is_araxl,
                nr_clusters=nr_clusters,
                ring_latency=ring_latency
            )

            # Assign the ARA Latency Model with AraXL awareness
            # MinorCPU doesn't natively use a LatencyModel object in the same way O3 does,
            # but we attach it here for consistency and future use in the timing model.
            self.latency_model = AraLatencyModel(
                simd_units=simd_units, 
                is_araxl=is_araxl,
                nr_clusters=nr_clusters,
                ring_latency=ring_latency
            )
            
except ImportError:
    pass
