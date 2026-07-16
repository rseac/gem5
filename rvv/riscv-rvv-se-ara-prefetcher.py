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
import os
import sys

sys.path.append(
    os.path.join(os.path.dirname(os.path.abspath(__file__)), "../src")
)
sys.path.append(
    os.path.join(os.path.dirname(os.path.abspath(__file__)), "/gem5/src")
)
# For sibling modules in rvv/ (vector_cache_hierarchy)
sys.path.append(os.path.dirname(os.path.abspath(__file__)))


import gem5.resources.resource as res
from gem5.components.boards.simple_board import SimpleBoard
from gem5.components.cachehierarchies.classic.private_l1_private_l2_cache_hierarchy import (
    PrivateL1PrivateL2CacheHierarchy,
)

# from gem5.components.memory import SingleChannelDDR3_1600
from gem5.components.memory import SingleChannelDDR4_2400
from gem5.components.processors.base_cpu_core import BaseCPUCore
from gem5.components.processors.base_cpu_processor import BaseCPUProcessor
from gem5.isas import ISA
from gem5.resources.resource import obtain_resource
from gem5.simulate.simulator import Simulator
from gem5.utils.requires import requires

# from cpu.o3.AraConfig import AraFUPool # Removed


class RVVCore(BaseCPUCore):
    def __init__(
        self,
        elen,
        vlen,
        cpu_id,
        enable_chaining,
        vector_timing_throughput,
        simd_units,
        scalar_uncacheable=False,
    ):
        # Use our custom SelectedCPU which handles FUPool configuration automatically
        core = SelectedCPU(cpu_id=cpu_id)

        # Configure the CPU Core
        core.enable_vector_chaining = enable_chaining
        core.vector_timing_throughput = vector_timing_throughput
        core.simd_units = simd_units
        core.scalar_uncacheable = scalar_uncacheable

        # --- MODULAR LATENCY MODEL ---
        # The AraO3CPU constructor automatically sets up the AraLatencyModel.
        # This provides a hook for researchers to swap in different timing models
        # without recompiling gem5.
        # -----------------------------

        super().__init__(core=core, isa=ISA.RISCV)

        # --- CRITICAL FIX: Propagate to ISA ---
        # The C++ vector timing model looks at the ISA objects, not the CPU.
        for isa in self.core.isa:
            isa.elen = elen
            isa.vlen = vlen
            # Ensure both possible parameter names are set for compatibility
            if hasattr(isa, "vector_timing_throughput"):
                isa.vector_timing_throughput = vector_timing_throughput
            if hasattr(isa, "timing_vector_throughput"):
                isa.timing_vector_throughput = vector_timing_throughput
            if hasattr(isa, "enable_chaining"):
                isa.enable_chaining = enable_chaining


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
parser.add_argument("resource", type=str)  # , choices=resources)
parser.add_argument("-c", "--cores", required=False, type=int, default=1)
parser.add_argument("-v", "--vlen", required=False, type=int, default=256)
parser.add_argument(
    "-e", "--elen", required=False, type=int, default=64
)  # spec only allows 64 bit elen
parser.add_argument("-d", "--l1d", required=False, type=str, default="32KiB")
parser.add_argument("-2", "--l2", required=False, type=str, default="512KiB")

parser.add_argument("-p", "--parms", required=False, type=str, default="2048")
parser.add_argument(
    "--cpu-type",
    type=str,
    default="AraO3",
    choices=["AraO3", "AraMinor"],
    help="CPU model to use: AraO3 (O3CPU) or AraMinor (MinorCPU)",
)
parser.add_argument(
    "--enable-chaining",
    action="store_true",
    default=True,
    help="Enable vector chaining",
)
parser.add_argument(
    "--disable-chaining",
    action="store_false",
    dest="enable_chaining",
    help="Disable vector chaining",
)
parser.add_argument(
    "--vector-timing-throughput",
    type=int,
    default=4,
    help="Number of elements per cycle for timing model",
)
parser.add_argument(
    "--simd-units", type=int, default=2, help="Number of physical SIMD lanes"
)
parser.add_argument(
    "--scalar-uncacheable",
    action="store_true",
    default=False,
    help="Mark scalar (non-vector) data accesses uncacheable so they "
    "bypass L1/L2, leaving the caches and prefetcher driven only by "
    "vector accesses (SE mode only; atomics/LR-SC stay cacheable)",
)
parser.add_argument(
    "--vector-cache",
    action="store_true",
    default=False,
    help="Give vector memory accesses their own private L1D+L2 chain "
    "in parallel with the scalar caches, steered by a VectorSplitter "
    "on the dcache port (coherent via the membus)",
)
parser.add_argument(
    "--vector-l1d",
    type=str,
    default="32KiB",
    help="Size of the vector-side L1D cache (with --vector-cache)",
)
parser.add_argument(
    "--vector-l2",
    type=str,
    default="512KiB",
    help="Size of the vector-side L2 cache (with --vector-cache)",
)
parser.add_argument(
    "--l1d-mshrs",
    type=int,
    default=16,
    help="MSHR count of the scalar L1D (with --vector-cache); bounds "
    "how many outstanding line misses the cache can overlap",
)
parser.add_argument(
    "--l2-mshrs",
    type=int,
    default=20,
    help="MSHR count of the scalar L2 (with --vector-cache)",
)
parser.add_argument(
    "--vector-l1d-mshrs",
    type=int,
    default=16,
    help="MSHR count of the vector L1D (with --vector-cache). Lowering "
    "this chokes the miss-level parallelism that hides gather-element "
    "miss latency, making gathers latency-bound",
)
parser.add_argument(
    "--vector-l2-mshrs",
    type=int,
    default=20,
    help="MSHR count of the vector L2 (with --vector-cache)",
)
parser.add_argument(
    "--l1d-tgts-per-mshr",
    type=int,
    default=20,
    help="Demand targets that can coalesce on one scalar-L1D MSHR "
    "(with --vector-cache)",
)
parser.add_argument(
    "--l2-tgts-per-mshr",
    type=int,
    default=12,
    help="Demand targets per scalar-L2 MSHR (with --vector-cache)",
)
parser.add_argument(
    "--vector-l1d-tgts-per-mshr",
    type=int,
    default=20,
    help="Demand targets per vector-L1D MSHR (with --vector-cache); "
    "gathers coalesce many same-line element accesses on one MSHR",
)
parser.add_argument(
    "--vector-l2-tgts-per-mshr",
    type=int,
    default=12,
    help="Demand targets per vector-L2 MSHR (with --vector-cache)",
)
parser.add_argument(
    "--prefetcher",
    type=str,
    default=None,
    choices=["none", "stride", "imp", "vimp", "isb", "stems"],
    help="Attach a hardware prefetcher to one cache. Implies "
    "(forces) --vector-cache. By default no cache has a prefetcher. "
    "Use --prefetcher-side and --prefetcher-level to place it. "
    "'vimp' is this fork's vector indirect memory prefetcher; it "
    "trains on virtual addresses, so the CPU MMU is registered on it "
    "automatically (override with --pf-param use_virtual_addresses=false).",
)
parser.add_argument(
    "--prefetcher-side",
    type=str,
    default="vector",
    choices=["scalar", "vector"],
    help="Which cache chain the prefetcher attaches to: vector = the "
    "vector-only L1D/L2 chain (default), scalar = the scalar L1D/L2 "
    "chain. Combine with --prefetcher-level to pick L1 vs L2.",
)
parser.add_argument(
    "--prefetcher-level",
    type=str,
    default="l2",
    choices=["l1", "l2"],
    help="Which cache the prefetcher attaches to on the chosen side: "
    "l1 = L1D, l2 = L2 (default l2)",
)
parser.add_argument(
    "--pf-param",
    action="append",
    default=None,
    metavar="NAME=VALUE",
    help="Tunable prefetcher parameter override (repeatable), e.g. "
    "--pf-param max_prefetch_distance=32. NAME is any Param.* on the "
    "selected prefetcher's class (see MEMORY_CONFIG.md).",
)

args = parser.parse_args()

# --- Prefetcher selection (configuration only; builds on --vector-cache) ---
# Parse repeated --pf-param NAME=VALUE into a dict of raw strings; the factory
# coerces each value to the type the Param expects.
pf_params = {}
for item in args.pf_param or []:
    if "=" not in item:
        print(f"Error: --pf-param expects NAME=VALUE, got '{item}'")
        sys.exit(1)
    key, value = item.split("=", 1)
    pf_params[key] = value

prefetcher_active = args.prefetcher not in (None, "none")
if pf_params and not prefetcher_active:
    print("Error: --pf-param requires --prefetcher (and not 'none')")
    sys.exit(1)

scalar_l1d_prefetcher = None
scalar_l2_prefetcher = None
vector_l1d_prefetcher = None
vector_l2_prefetcher = None
prefetcher_mmu = False
if prefetcher_active:
    # The split hierarchy hosts the prefetcher on whichever chain is
    # selected, so it is required.
    args.vector_cache = True
    from prefetcher_factory import build as build_prefetcher
    from prefetcher_factory import needs_mmu as prefetcher_needs_mmu

    try:
        factory = build_prefetcher(args.prefetcher, pf_params)
    except ValueError as exc:
        print(f"Error: {exc}")
        sys.exit(1)
    # Virtual-address prefetchers (vimp by default) need the CPU MMU to
    # translate page-crossing prefetch targets.
    prefetcher_mmu = prefetcher_needs_mmu(args.prefetcher, pf_params)
    # Route the single factory to one of four caches: {scalar,vector} x {l1,l2}.
    if args.prefetcher_side == "scalar":
        if args.prefetcher_level == "l1":
            scalar_l1d_prefetcher = factory
        else:
            scalar_l2_prefetcher = factory
    else:
        if args.prefetcher_level == "l1":
            vector_l1d_prefetcher = factory
        else:
            vector_l2_prefetcher = factory

# Import the selected CPU model
if args.cpu_type == "AraO3":
    from cpu.o3.AraConfig import AraO3CPU as SelectedCPU
elif args.cpu_type == "AraMinor":
    from cpu.minor.AraMinorConfig import AraMinorCPU as SelectedCPU
else:
    print(f"Error: Unknown CPU type {args.cpu_type}")
    sys.exit(1)

if args.vector_cache:
    from vector_cache_hierarchy import VectorSplitCacheHierarchy

    cache_hierarchy = VectorSplitCacheHierarchy(
        l1d_size=args.l1d,
        l1i_size="32KiB",
        l2_size=args.l2,
        vector_l1d_size=args.vector_l1d,
        vector_l2_size=args.vector_l2,
        scalar_l1d_prefetcher=scalar_l1d_prefetcher,
        scalar_l2_prefetcher=scalar_l2_prefetcher,
        vector_l1d_prefetcher=vector_l1d_prefetcher,
        vector_l2_prefetcher=vector_l2_prefetcher,
        prefetcher_needs_mmu=prefetcher_mmu,
        l1d_mshrs=args.l1d_mshrs,
        l2_mshrs=args.l2_mshrs,
        vector_l1d_mshrs=args.vector_l1d_mshrs,
        vector_l2_mshrs=args.vector_l2_mshrs,
        l1d_tgts_per_mshr=args.l1d_tgts_per_mshr,
        l2_tgts_per_mshr=args.l2_tgts_per_mshr,
        vector_l1d_tgts_per_mshr=args.vector_l1d_tgts_per_mshr,
        vector_l2_tgts_per_mshr=args.vector_l2_tgts_per_mshr,
    )
else:
    cache_hierarchy = PrivateL1PrivateL2CacheHierarchy(
        # l1d_size="32KiB", l1i_size="32KiB", l2_size="512KiB"
        l1d_size=args.l1d,
        l1i_size="32KiB",
        l2_size=args.l2,
    )

# memory = SingleChannelDDR3_1600()
memory = SingleChannelDDR4_2400(size="8GiB")

processor = BaseCPUProcessor(
    cores=[
        RVVCore(
            args.elen,
            args.vlen,
            i,
            args.enable_chaining,
            args.vector_timing_throughput,
            args.simd_units,
            args.scalar_uncacheable,
        )
        for i in range(args.cores)
    ]
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
print(
    f"  Vector Chaining:  {'ENABLED' if args.enable_chaining else 'DISABLED'}"
)
print(f"  Throughput:       {args.vector_timing_throughput} elements/cycle")
print(f"  SIMD Units:       {args.simd_units} parallel units")
print(f"  L1D Cache:        {args.l1d}")
print(f"  L2 Cache:         {args.l2}")
print(
    f"  Scalar Bypass:    "
    f"{'ON (scalar accesses uncacheable)' if args.scalar_uncacheable else 'OFF'}"
)
if args.vector_cache:
    print(f"  Vector Caches:    ON (split hierarchy via VectorSplitter)")
    print(f"  Vector L1D Cache: {args.vector_l1d}")
    print(f"  Vector L2 Cache:  {args.vector_l2}")
    if prefetcher_active:
        side = "vector" if args.prefetcher_side == "vector" else "scalar"
        cache = "L1D" if args.prefetcher_level == "l1" else "L2"
        print(f"  Prefetcher:       {args.prefetcher} on {side} {cache}")
        print(
            f"  PF MMU:           "
            f"{'registered (VA training, page-crossing OK)' if prefetcher_mmu else 'none (PA training, page-crossing dropped)'}"
        )
        if pf_params:
            print(f"  PF Params:        {pf_params}")
    else:
        print(f"  Prefetcher:       none (all caches prefetcher-free)")
else:
    print(f"  Vector Caches:    OFF (shared L1D/L2)")
print("-" * 50)
print("Beginning simulation...")
print("=" * 50)

# board.set_se_binary_workload(binary, arguments=[args.parms])
board.set_se_binary_workload(binary, arguments=args.parms.split())

import m5  # For curTick()

simulator = Simulator(board=board, full_system=False)

simulator.run()

# Output cycles (assuming 1GHz clock as configured in SimpleBoard)
# 1GHz = 1000 ps period (default gem5 tick is 1ps)
cycles = int(m5.curTick() / 1000)

print("\n" + "=" * 50)
print("      ARA RISC-V VECTOR SIMULATION RESULTS")
print("-" * 50)
print(f"  CPU Model:            {args.cpu_type}")
print(
    f"  Vector Chaining:      {'ENABLED' if args.enable_chaining else 'DISABLED'}"
)
print(f"  Total Ticks:          {m5.curTick()} ps")
print(f"  Total Execution Cycles: {cycles}")
print("=" * 50 + "\n")
