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
from m5.objects.FUPool import FUPool
from m5.params import *
from m5.SimObject import SimObject

class AraVALU(FUDesc):
    """
    ARA Integer ALU VFU.
    Handles integer arithmetic, logic, shifts, compare, and config.
    count=1: one instruction enters this pipeline per cycle.
    Two independent vadd/vsub cannot issue in the same cycle.
    """
    opList = [
        OpDesc(opClass="SimdAdd",      opLat=1),
        OpDesc(opClass="SimdAddAcc",   opLat=1),
        OpDesc(opClass="SimdAlu",      opLat=1),
        OpDesc(opClass="SimdCmp",      opLat=1),
        OpDesc(opClass="SimdCvt",      opLat=1),
        OpDesc(opClass="SimdMisc",     opLat=1),
        OpDesc(opClass="SimdShift",    opLat=1),
        OpDesc(opClass="SimdShiftAcc", opLat=1),
        # vsetvl/vsetvli — programmes the vector length register
        OpDesc(opClass="SimdConfig",   opLat=1),
    ]
    count = 1


class AraVMFPU(FUDesc):
    """
    ARA FP+Multiply VFU (fpnew).
    Handles integer multiply and all FP arithmetic, compare, and convert.
    count=1: one instruction per cycle. Two independent vfadd/vfmul cannot
    issue in the same cycle (unlike the old AraSIMD_Pipelined(count=2)).
    """
    opList = [
        # Integer multiply (shares fpnew datapath in ARA)
        OpDesc(opClass="SimdMult",           opLat=1),
        OpDesc(opClass="SimdMultAcc",        opLat=1),
        OpDesc(opClass="SimdMatMultAcc",     opLat=1),
        # FP arithmetic — opLat matches AraLatencyModel pipeline depths
        OpDesc(opClass="SimdFloatAdd",       opLat=4),
        OpDesc(opClass="SimdFloatAlu",       opLat=4),
        OpDesc(opClass="SimdFloatMult",      opLat=4),
        OpDesc(opClass="SimdFloatMultAcc",   opLat=4),
        OpDesc(opClass="SimdFloatMatMultAcc",opLat=4),
        # FP compare / misc / convert
        OpDesc(opClass="SimdFloatCmp",       opLat=1),
        OpDesc(opClass="SimdFloatMisc",      opLat=1),
        OpDesc(opClass="SimdFloatCvt",       opLat=2),
    ]
    count = 1


class AraVLSU(FUDesc):
    """
    ARA Load/Store VFU (VLSU): unit-stride and whole-register memory ops.
    Loads and stores share this single pipelined unit — ARA has one AGU.
    count=1: a vle and a vse cannot issue in the same cycle.
    """
    opList = [
        OpDesc(opClass="SimdUnitStrideLoad",           opLat=3),
        OpDesc(opClass="SimdUnitStrideStore",          opLat=3),
        OpDesc(opClass="SimdUnitStrideMaskLoad",       opLat=3),
        OpDesc(opClass="SimdUnitStrideMaskStore",      opLat=3),
        OpDesc(opClass="SimdUnitStrideSegmentedLoad",  opLat=3),
        OpDesc(opClass="SimdUnitStrideSegmentedStore", opLat=3),
        OpDesc(opClass="SimdWholeRegisterLoad",        opLat=3),
        OpDesc(opClass="SimdWholeRegisterStore",       opLat=3),
    ]
    count = 1


class AraVSLD(FUDesc):
    """
    ARA Slide/Permute/Strided/Indexed VFU.
    Covers non-unit-stride and indexed memory, element permutations
    (vslide, vrgather), and reductions (which use the cross-lane slide network).
    count=1: single pipeline.
    """
    opList = [
        OpDesc(opClass="SimdStridedLoad",    opLat=1),
        OpDesc(opClass="SimdStridedStore",   opLat=1),
        OpDesc(opClass="SimdIndexedLoad",    opLat=1),
        OpDesc(opClass="SimdIndexedStore",   opLat=1),
        # Element permutation (vslide, vrgather, vext)
        OpDesc(opClass="SimdExt",            opLat=1),
        OpDesc(opClass="SimdFloatExt",       opLat=1),
        # Reductions traverse all lane results via the slide network
        OpDesc(opClass="SimdReduceAdd",      opLat=1),
        OpDesc(opClass="SimdReduceAlu",      opLat=1),
        OpDesc(opClass="SimdReduceCmp",      opLat=1),
        OpDesc(opClass="SimdFloatReduceAdd", opLat=1),
        OpDesc(opClass="SimdFloatReduceCmp", opLat=1),
    ]
    count = 1


class AraIntDiv(FUDesc):
    """
    ARA integer divide unit — non-pipelined, count=1.
    ARA has one serial integer divider; a second vdiv must wait.
    """
    opList = [
        # RTL: bit-serial divider, depth = 8<<vsew + 9 (73 cycles for EW64)
        OpDesc(opClass="SimdDiv", opLat=73, pipelined=False),
    ]
    count = 1


class AraFPDivSqrt(FUDesc):
    """
    ARA FP divide/sqrt unit (fpnew DIVSQRT) — non-pipelined, count=1.
    Iterative SRT: only one vfdiv or vfsqrt can be in-flight at a time.
    """
    opList = [
        OpDesc(opClass="SimdFloatDiv",  opLat=20, pipelined=False),
        OpDesc(opClass="SimdFloatSqrt", opLat=20, pipelined=False),
    ]
    count = 1


# ---------------------------------------------------------------------------
# Backward-compatible aliases — scripts using the old names still work
# ---------------------------------------------------------------------------
AraSIMD_Pipelined = AraVLSU    # closest single-class analogue; prefer per-VFU classes
AraSIMD_IntDiv    = AraIntDiv
AraSIMD_FPDivSqrt = AraFPDivSqrt


from m5.objects.LatencyModel import LatencyModel
from m5.objects.FuncUnit import OpClass

class AraLatencyModel(LatencyModel):
    def __init__(self, simd_units=2, **kwargs):
        super().__init__(**kwargs)

        # Dynamic Dispatch Floor: max(4, 12 - L)
        # This accounts for the core-to-vector handshake becoming a smaller
        # percentage of total execution as the vector units grow.
        self.dispatchFloor = max(4, 12 - simd_units)

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
            
            # Memory (AGU depth = 3)
            lats[m['SimdUnitStrideLoad']] = 3
            lats[m['SimdUnitStrideStore']] = 3

            # Permute / Reduction
            lats[m['SimdReduceAdd']] = 1
            lats[m['SimdFloatReduceAdd']] = 1
            
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
        def __init__(self, simd_units=2, **kwargs):
            # Proportional Scaling: Each 2 lanes add 1 wide to the scalar core
            # 2 lanes -> 1 wide, 4 lanes -> 2 wide, 8 lanes -> 4 wide
            scale = max(1, simd_units // 2)

            # --- Scaled Pipeline Widths ---
            self.fetchWidth = scale
            self.decodeWidth = scale
            self.renameWidth = scale
            self.dispatchWidth = scale
            self.issueWidth = scale
            self.wbWidth = scale
            self.commitWidth = scale
            self.squashWidth = scale

            # --- Scaled Core Buffer Sizes ---
            # We scale buffers to ensure the wider front-end doesn't cause
            # "resource full" stalls before the lanes are saturated.
            self.numROBEntries = 32 * scale
            self.numPhysIntRegs = 64 * scale
            self.numPhysFloatRegs = 64 * scale
            self.LQEntries = 16 * scale
            self.SQEntries = 16 * scale

            super().__init__(**kwargs)

            # Assign the ARA Latency Model with lane-aware dispatch floor
            self.latency_model = AraLatencyModel(simd_units=simd_units)

            # --- Serialize Cache Ports ---
            # Even with multiple lanes, requests are serialized at the cache level
            # to match ARA's single-ported memory model.
            self.cacheStorePorts = 1
            self.cacheLoadPorts = 1

            # Increase TimeBuffer sizes to ensure they are deep enough
            # for long ARA RTL latencies (e.g., 74-cycle division).
            self.backComSize = 100
            self.forwardComSize = 100

            # CRITICAL: In gem5, SimObject instances (like functional units) must
            # have a clear parent-child relationship. We instantiate them inside
            # the constructor so they are immediately attached to their parents.
            for iq in self.instQueues:
                # Set IQ size proportional to scale (Deeper for wider lanes)
                iq.numEntries = 32 * scale

                # Per-VFU FU pool — one descriptor per ARA hardware VFU, each
                # with count=1 (pipelined). This correctly enforces:
                #   AraVALU    : one integer ALU op per cycle
                #   AraVMFPU   : one FP/mul op per cycle  (fixes the old bug
                #                where count=2 allowed 2 independent vfadd/cycle)
                #   AraVLSU    : one unit-stride load OR store per cycle
                #   AraVSLD    : one strided/indexed/slide/reduction per cycle
                #   AraIntDiv  : serial integer divide (non-pipelined)
                #   AraFPDivSqrt: iterative FP div/sqrt (non-pipelined)
                # Cross-VFU parallelism is still possible: a vfadd (AraVMFPU)
                # and a vle16 (AraVLSU) CAN issue in the same cycle.
                # Chaining still works: pipelined slots are freed next cycle,
                # before WakeDependents fires, so the consumer acquires the
                # freed slot without needing a second permanent slot.
                iq.fuPool = FUPool(FUList=[
                    IntALU(), IntMultDiv(), FP_ALU(), FP_MultDiv(),
                    ReadPort(),
                    AraVALU(),
                    AraVMFPU(),
                    AraVLSU(),
                    AraVSLD(),
                    AraIntDiv(),
                    AraFPDivSqrt(),
                    Matrix_Unit(), System_Unit(), PredALU(),
                    WritePort(), RdWrPort()
                ])

except ImportError:
    pass
