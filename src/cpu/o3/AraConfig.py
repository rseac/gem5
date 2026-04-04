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
    opList = [
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

    count = 2


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


try:
    from m5.objects import RiscvO3CPU
    class AraO3CPU(RiscvO3CPU):
        """
        Custom RiscvO3CPU that automatically sets up the ARA Functional Unit Pool
        and constrains widths to match ARA hardware.
        """
        # Constrain O3 pipeline widths to match ARA's 2-lane issue capability
        fetchWidth = 2
        decodeWidth = 2
        renameWidth = 2
        dispatchWidth = 2
        issueWidth = 2
        wbWidth = 2
        commitWidth = 2
        squashWidth = 2

        def __init__(self, **kwargs):
            super().__init__(**kwargs)

            # CRITICAL: In gem5, SimObject instances (like functional units) must
            # have a clear parent-child relationship. We instantiate them inside
            # the constructor so they are immediately attached to their parents.
            for iq in self.instQueues:
                # We provide a fresh set of functional units for every Instruction Queue (IQ).
                # This prevents 'multiple parent' and 'orphan node' RuntimeErrors.
                #
                # AraSIMD_Pipelined: count=simd_units (default 2) — the two slots
                #   needed for gem5's WakeDependents chaining mechanism.
                # AraSIMD_IntDiv / AraSIMD_FPDivSqrt: count=1 — enforces the
                #   single-issue structural hazard on ARA's serial divide units.
                iq.fuPool = FUPool(FUList = [
                    IntALU(), IntMultDiv(), FP_ALU(), FP_MultDiv(),
                    ReadPort(),
                    AraSIMD_Pipelined(count=self.simd_units),
                    AraSIMD_IntDiv(),
                    AraSIMD_FPDivSqrt(),
                    Matrix_Unit(), System_Unit(), PredALU(),
                    WritePort(), RdWrPort()
                ])

except ImportError:
    pass
