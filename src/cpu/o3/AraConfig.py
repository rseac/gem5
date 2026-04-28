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
from m5.objects.FuncUnit import *
from m5.objects.FuncUnitConfig import *
from m5.objects.FUPool import FUPool
from m5.params import *
from m5.SimObject import SimObject

class AraSIMD_Pipelined(FUDesc):
    """
    Pipelined ARA vector functional units: ALU, Multiply, FP compute, conversions,
    reductions, and load/store address generation.

    count=simd_units (default 2) provides the two FU slots required by gem5's O3
    chaining mechanism — one for the producer instruction still occupying the
    pipeline, one for the consumer that starts early via WakeDependents.  This
    mirrors the ARA hardware behaviour where a new instruction can enter a
    pipelined VFU while the previous one is still streaming elements through.
    """
    def __init__(self, count=2, op_lat_mem=3, **kwargs):
        super().__init__(**kwargs)
        self.count = count

        self.opList = [
            # --- Integer Arithmetic ---
            OpDesc(opClass="SimdAdd", opLat=1),
            OpDesc(opClass="SimdAddAcc", opLat=1),
            OpDesc(opClass="SimdAlu", opLat=1),
            OpDesc(opClass="SimdCmp", opLat=1),
            OpDesc(opClass="SimdCvt", opLat=1),
            OpDesc(opClass="SimdMisc", opLat=1),
            OpDesc(opClass="SimdShift", opLat=1),
            OpDesc(opClass="SimdShiftAcc", opLat=1),

            # --- Integer Multiply ---
            OpDesc(opClass="SimdMult", opLat=1),
            OpDesc(opClass="SimdMultAcc", opLat=1),
            OpDesc(opClass="SimdMatMultAcc", opLat=1),

            # --- Float Arithmetic ---
            # RTL: LatFCompEW64=5, LatFCompEW32=4, LatFCompEW16=3, LatFCompEW8=2
            # gem5 opClass does not distinguish element width, so opLat=4 is used as
            # the EW32 (single-precision) representative — the dominant FP width in
            # practice. EW64 workloads will see 1-cycle optimism; EW16/8 workloads
            # will see 1-2 cycles pessimism.
            OpDesc(opClass="SimdFloatAdd", opLat=10),
            OpDesc(opClass="SimdFloatAlu", opLat=10),
            OpDesc(opClass="SimdFloatMult", opLat=10),
            OpDesc(opClass="SimdFloatMultAcc", opLat=10),
            OpDesc(opClass="SimdFloatMatMultAcc", opLat=10),

            # --- Float Misc / Compare ---
            OpDesc(opClass="SimdFloatCmp", opLat=1),
            OpDesc(opClass="SimdFloatMisc", opLat=1),

            # --- Float Conversion ---
            OpDesc(opClass="SimdFloatCvt", opLat=2),

            # --- Reductions ---
            OpDesc(opClass="SimdReduceAdd", opLat=1),
            OpDesc(opClass="SimdReduceAlu", opLat=1),
            OpDesc(opClass="SimdReduceCmp", opLat=1),
            OpDesc(opClass="SimdFloatReduceAdd", opLat=1),
            OpDesc(opClass="SimdFloatReduceCmp", opLat=1),

            # --- Load / Store Address Generation ---
            # Increased to 3 cycles (Ara) or 6 cycles (AraXL) to account for 
            # internal shuffling and AXI request generation logic.
            OpDesc(opClass="SimdUnitStrideLoad", opLat=op_lat_mem),
            OpDesc(opClass="SimdUnitStrideStore", opLat=op_lat_mem),
            OpDesc(opClass="SimdUnitStrideMaskLoad", opLat=op_lat_mem),
            OpDesc(opClass="SimdUnitStrideMaskStore", opLat=op_lat_mem),
            OpDesc(opClass="SimdStridedLoad", opLat=1),
            OpDesc(opClass="SimdStridedStore", opLat=1),
            OpDesc(opClass="SimdIndexedLoad", opLat=1),
            OpDesc(opClass="SimdIndexedStore", opLat=1),
            OpDesc(opClass="SimdWholeRegisterLoad", opLat=op_lat_mem),
            OpDesc(opClass="SimdWholeRegisterStore", opLat=op_lat_mem),
            OpDesc(opClass="SimdUnitStrideSegmentedLoad", opLat=op_lat_mem),
            OpDesc(opClass="SimdUnitStrideSegmentedStore", opLat=op_lat_mem),
            OpDesc(opClass="SimdExt", opLat=1),
            OpDesc(opClass="SimdFloatExt", opLat=1),
            OpDesc(opClass="SimdConfig", opLat=1),
        ]


class AraSIMD_IntDiv(FUDesc):
    """
    ARA integer divide unit — non-pipelined, count=1.

    ARA has one serial integer divider shared across all lanes.  A second
    independent vdiv cannot start until the first completes.  count=1 enforces
    this structural hazard; pipelined=False prevents micro-op overlap within a
    single instruction.
    """
    opList = [
        # RTL: serial divider, pipeline depth = 8 << vsew + 9 (73 for EW64).
        OpDesc(opClass="SimdDiv", opLat=73, pipelined=False),
    ]

    count = 1


class AraSIMD_FPDivSqrt(FUDesc):
    """
    ARA FP divide / square-root unit — non-pipelined, count=1.

    fpnew's DIVSQRT unit is iterative: only one FP divide or sqrt can be
    in-flight per lane at a time, and all lanes share the same issue slot.
    count=1 models the single-issue constraint; pipelined=False prevents
    micro-op overlap within one instruction.
    """
    opList = [
        # RTL: LatFDivSqrt=3 (ara_pkg.sv:95) — but iterative compute time is ~17.
        OpDesc(opClass="SimdFloatDiv",  opLat=20, pipelined=False),
        OpDesc(opClass="SimdFloatSqrt", opLat=20, pipelined=False),
    ]

    count = 1


from m5.objects.LatencyModel import LatencyModel
from m5.objects.FuncUnit import OpClass

class AraLatencyModel(LatencyModel):
    def __init__(self, simd_units=2, is_araxl=False, nr_clusters=1,
                 ring_latency=2, **kwargs):
        super().__init__(**kwargs)

        # Dynamic Dispatch Floor: max(4, 12 - L)
        # This accounts for the core-to-vector handshake becoming a smaller
        # percentage of total execution as the vector units grow.
        # In AraXL, we increase the floor by 2 cycles to account for 
        # additional spill registers in the cva6_cut and macro interfaces.
        base_floor = max(4, 12 - simd_units)
        self.dispatchFloor = base_floor + (2 if is_araxl else 0)

        def get_lats(vsew):
            # Pre-populate with 0 (falls back to standard FU opLat if not defined here)
            # OpClass indices are stable in a given build.
            lats = [0] * 128 
            m = OpClass.map
            
            # Integer ALU (RTL Pipeline Depth = 1)
            lats[m['SimdAdd']]   = 1
            lats[m['SimdAlu']]   = 1
            lats[m['SimdShift']] = 1
            lats[m['SimdMisc']]  = 1
            lats[m['SimdCmp']]   = 1
            
            # Floating Point Arithmetic (RTL Pipeline Depths)
            # Hardware: e64=5, e32=4, e16=3, e8=2. 
            fp_pipe = vsew + 2
            lats[m['SimdFloatAdd']] = fp_pipe
            lats[m['SimdFloatAlu']] = fp_pipe
            lats[m['SimdFloatMult']] = fp_pipe
            lats[m['SimdFloatMultAcc']] = fp_pipe
            lats[m['SimdFloatMatMultAcc']] = fp_pipe
            
            # Floating Point Misc / Compare (RTL = 1)
            lats[m['SimdFloatCmp']] = 1
            lats[m['SimdFloatMisc']] = 1

            # Floating Point Conversion (RTL=2)
            lats[m['SimdFloatCvt']] = 2

            # Division / Sqrt (Iterative base cycles per element)
            # These values are multiplied by microVl in latency_model.cc
            lats[m['SimdFloatDiv']] = 17  # ~17 cycles iterative SRT
            lats[m['SimdFloatSqrt']] = 17 # ~17 cycles iterative SRT
            lats[m['SimdDiv']] = 8 << vsew # Bit-serial: ~8 cycles per bit of width

            # Integer Multiply (e8=0, others=1)

            lats[m['SimdMult']] = (0 if vsew == 0 else 1)
            lats[m['SimdMultAcc']] = (0 if vsew == 0 else 1)
            
            # Memory (AGU depth = 3 in Ara, 6 in AraXL)
            mem_agu_lat = 6 if is_araxl else 3
            lats[m['SimdUnitStrideLoad']] = mem_agu_lat
            lats[m['SimdUnitStrideStore']] = mem_agu_lat

            # Permute / Reduction
            # AraXL reduction latency accounts for inter-cluster reduction tree.
            # Each cluster hop costs ~2 * ring_latency cycles (round trip/sync).
            inter_cluster_overhead = 0
            if is_araxl and nr_clusters > 1:
                # Logarithmic reduction tree overhead
                inter_cluster_overhead = 2 * ring_latency * \
                                        math.ceil(math.log2(nr_clusters))
            
            lats[m['SimdReduceAdd']] = 1 + inter_cluster_overhead
            lats[m['SimdFloatReduceAdd']] = 1 + inter_cluster_overhead
            
            # For slides (Permute), overhead is proportional to ring distance.
            # We use an average distance of nr_clusters / 2 hops.
            permute_overhead = 0
            if is_araxl and nr_clusters > 1:
                permute_overhead = 2 * ring_latency * (nr_clusters // 2)

            lats[m['SimdMisc']] += permute_overhead

            return lats

        self.latenciesEW8  = get_lats(0)
        self.latenciesEW16 = get_lats(1)
        self.latenciesEW32 = get_lats(2)
        self.latenciesEW64 = get_lats(3)

try:
    from m5.objects import RiscvO3CPU
    class AraO3CPU(RiscvO3CPU):
        """
        Custom RiscvO3CPU that automatically sets up the ARA Functional Unit Pool
        and scales core resources based on the number of vector lanes.
        """
        def __init__(self, simd_units=2, nr_clusters=1, is_araxl=False, 
                     ring_latency=2, **kwargs):
            # Total ARA lanes = lanes_per_cluster (throughput) * nr_clusters
            # For AraO3, simd_units is always 2 (chaining support).
            # The actual lane count (throughput) is passed separately 
            # to the ISA model via vector-timing-throughput.
            
            # We assume the user provides total lanes via an external param
            # or it is inferred. Here we scale based on nr_clusters.
            # Proportional Scaling: Higher cluster count implies a beefier core.
            scale = max(1, nr_clusters)

            # --- Scaled Pipeline Widths ---
            self.fetchWidth = scale * 2
            self.decodeWidth = scale * 2
            self.renameWidth = scale * 2
            self.dispatchWidth = scale * 2
            self.issueWidth = scale * 2
            self.wbWidth = scale * 2
            self.commitWidth = scale * 2
            self.squashWidth = scale * 2

            # --- Scaled Core Buffer Sizes ---
            self.numROBEntries = 64 * scale
            self.numPhysIntRegs = 128 * scale
            self.numPhysFloatRegs = 128 * scale
            self.LQEntries = 32 * scale
            self.SQEntries = 32 * scale

            super().__init__(**kwargs)

            # Assign the ARA Latency Model with AraXL awareness
            self.latency_model = AraLatencyModel(
                simd_units=simd_units, 
                is_araxl=is_araxl,
                nr_clusters=nr_clusters,
                ring_latency=ring_latency
            )

            # --- Serialize Cache Ports ---
            self.cacheStorePorts = 1
            self.cacheLoadPorts = 1

            self.backComSize = 100
            self.forwardComSize = 100

            # Functional unit memory latency
            mem_lat = 6 if is_araxl else 3

            for iq in self.instQueues:
                iq.numEntries = 64 * scale

                iq.fuPool = FUPool(FUList = [
                    IntALU(), IntMultDiv(), FP_ALU(), FP_MultDiv(),
                    ReadPort(),
                    AraSIMD_Pipelined(count=simd_units, op_lat_mem=mem_lat),
                    AraSIMD_IntDiv(),
                    AraSIMD_FPDivSqrt(),
                    Matrix_Unit(), System_Unit(), PredALU(),
                    WritePort(), RdWrPort()
                ])

except ImportError:
    pass
