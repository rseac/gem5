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
parser.add_argument(
    "--mem-type",
    type=str,
    default="ddr4",
    choices=["ddr4", "perfect"],
    help="Main memory model: ddr4 = SingleChannelDDR4_2400 (default). "
    "perfect = SingleChannelSimpleMemory with 1ns fixed latency and "
    "1TiB/s bandwidth, so every cache miss is serviced almost for free; "
    "use it as the perfect-memory counterfactual when measuring a "
    "kernel's compute floor (fread-loaded benchmarks can't be warmed "
    "into a big L2 because gem5-SE services fread functionally).",
)

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
    "--unified-cache",
    action="store_true",
    default=False,
    help="Host the prefetcher on the classic shared L1D/L2 chain "
    "(no VectorSplitter: scalar and vector accesses share one L1D). "
    "Unlike the bare default hierarchy this strips the stdlib caches' "
    "built-in stride prefetchers, so a run without --prefetcher is a "
    "true no-prefetcher base, and it honors --l1d-mshrs/--l2-mshrs. "
    "gdp/viper/tyche work here because the shared L1D sees every "
    "access class; --prefetcher-side is ignored (there is only one "
    "chain), --prefetcher-level still picks L1D vs L2. Mutually "
    "exclusive with --vector-cache and --scalar-prefetcher.",
)
parser.add_argument(
    "--ara-cache",
    action="store_true",
    default=False,
    help="Ara-shaped hierarchy: the L1D is SCALAR-ONLY and the L2 is "
    "shared - vector accesses bypass the L1 and enter the L2 crossbar "
    "directly through the VectorSplitter (rvv/ara_cache_hierarchy.py), "
    "modeling CVA6's private L1D + Ara's VLSU-to-L2 path. Coherent by "
    "ordinary snooping on the L2 crossbar. Stdlib stride prefetchers "
    "are stripped, --l1d-mshrs/--l2-mshrs are honored. Vector-side "
    "prefetchers (gdp/viper/vtyche2/vhybrid/revela) must use "
    "--prefetcher-level l2 (the L1D never sees vector accesses); "
    "--prefetcher-side is ignored. --scalar-prefetcher may attach to "
    "the scalar L1D alongside. Mutually exclusive with --vector-cache "
    "and --unified-cache.",
)
parser.add_argument(
    "--ara-vbuf-size",
    type=str,
    default="512B",
    help="(--ara-cache) Size of the tiny 1-cycle vector buffer between "
    "the splitter and the L2 crossbar. It is a protocol shim (the O3 "
    "LSQ needs a cache as its direct memory peer) that doubles as the "
    "VLSU request-buffer model; at the default 8 lines it is far too "
    "small to act as a data cache.",
)
parser.add_argument(
    "--ara-vbuf-mshrs",
    type=int,
    default=16,
    help="(--ara-cache) Vector buffer MSHRs: bounds the vector unit's "
    "outstanding memory transactions (the VLSU's AXI depth).",
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
    choices=["none", "stride", "imp", "vimp", "gdp", "viper",
             "vtyche2", "isb", "stems", "tyche", "revela", "vhybrid"],
    help="Attach a hardware prefetcher to one cache. Implies "
    "(forces) --vector-cache. By default no cache has a prefetcher. "
    "Use --prefetcher-side and --prefetcher-level to place it. "
    "'vimp' (chunk-trained indirect), 'gdp' (Gather Dataflow "
    "Prefetcher: architectural transform-chain replay, exact "
    "A[f(B[i])] plus vector streaming), 'viper' (VIPER: gdp's "
    "architectural discovery with imp's base+(index<<shift) equation, "
    "A[B[i]] only, converted a whole index line at a time), "
    "'revela' (ReVeLA ICS'24: unit-stride streams announced by the "
    "vsetvl AVL, every-cycle trigger, near-perfect accuracy), "
    "'vhybrid' (revela's announcement-driven streams + viper's "
    "capture/convert indirect half; needs both sideband tables, "
    "wired automatically) "
    "and 'tyche' (scalar dependency-chain replay) are this "
    "fork's prefetchers; they train on virtual addresses, so the CPU "
    "MMU is registered automatically (override with --pf-param "
    "use_virtual_addresses=false). gdp, viper and revela need "
    "--prefetcher-side vector; tyche needs --prefetcher-side scalar "
    "(it decodes scalar instructions, which never reach the vector "
    "caches).",
)
parser.add_argument(
    "--revela-stt-entries",
    type=int,
    default=16,
    help="ReVeLA only: Stream Tracking Table entries on the CPU-side "
    "RevelaStreamTable (paper default 16, sensitivity-swept 1-32; 4 "
    "already reaches 91%% of the benefit). Other ReVeLA knobs "
    "(max_prefetch_distance, degree, queue_size) are "
    "prefetcher params: use --pf-param.",
)
parser.add_argument(
    "--vector-dct-entries",
    type=int,
    default=8,
    help="gdp/viper/vtyche2/vhybrid: Dependency Chain Table entries on "
    "the CPU-side VectorChainTable (default 8, minimum 3). One table is "
    "built per core and shared by whichever of those prefetchers is "
    "attached, so this sizes vhybrid's indirect half too. The whole "
    "table clears when full, and it also bounds the consumer->head walk "
    "length, so a small DCT both wipes learned chains more often and "
    "caps how far back a walk may reach. Storage accounting in "
    "src/cpu/vector_chain_table.hh assumes 8 (3-bit pointers); the sim "
    "stores pointers as int, so larger values run but cost more "
    "hardware than that note claims.",
)
parser.add_argument(
    "--vector-max-transform-stages",
    type=int,
    default=4,
    help="gdp/viper/vtyche2/vhybrid: maximum transform links between a "
    "producer index load and the gather it feeds (VectorChainTable "
    "max_transform_stages, default 4). Chains longer than this do not "
    "link at all, so raising --vector-dct-entries alone will not admit "
    "longer transform chains.",
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
parser.add_argument(
    "--scalar-prefetcher",
    type=str,
    default=None,
    choices=["none", "stride", "imp", "isb", "stems", "tyche"],
    help="Attach a SECOND, independent prefetcher to the scalar L1D, "
    "alongside whatever --prefetcher places on the vector side. Implies "
    "--vector-cache. For a scalar-only prefetcher either use this with "
    "--prefetcher none, or use the existing --prefetcher-side scalar "
    "(combining --prefetcher-side scalar with this option is an error: "
    "both would claim the scalar chain). gdp and viper are not "
    "accepted here (their CPU-side records only reach vector-side "
    "caches); tyche is (it is a scalar-side design).",
)
parser.add_argument(
    "--scalar-pf-param",
    action="append",
    default=None,
    metavar="NAME=VALUE",
    help="Tunable parameter override for --scalar-prefetcher "
    "(repeatable), same semantics as --pf-param.",
)
parser.add_argument(
    "--l2-prefetcher",
    type=str,
    default=None,
    choices=["none", "stride"],
    help="Attach a SECOND, independent prefetcher to the unified L2, "
    "alongside the --prefetcher on the L1D (--unified-cache with "
    "--prefetcher-level l1 only). Covers the traffic classes an "
    "L1-hosted vector prefetcher never learns — the scalar row-pointer "
    "walk and store streams that miss through to the L2.",
)
parser.add_argument(
    "--l2-pf-param",
    action="append",
    default=None,
    metavar="NAME=VALUE",
    help="Tunable parameter override for --l2-prefetcher "
    "(repeatable), same semantics as --pf-param.",
)
parser.add_argument(
    "--stream-demote",
    type=str,
    default="none",
    choices=["none", "l1", "l2", "both"],
    help="Stream-aware cache replacement (needs a vector-chain-table "
    "prefetcher; on --unified-cache it applies to the shared caches, "
    "on the split hierarchy to the VECTOR chain): demote lines of the "
    "prefetcher-identified index/data stream pages to the LRU "
    "position so single-use stream traffic stops evicting reused "
    "data (e.g. spmv's gather-target vector). l2 demotes at "
    "insertion (the L2 copy of a stream line is dead on arrival), "
    "l1 demotes at first touch (the line still owes its one use), "
    "both applies each mode at its level.",
)
parser.add_argument(
    "--stream-demote-demand",
    action="store_true",
    help="Feed the stream-page registry from DEMAND accesses: every "
    "unit-stride vector load/store registers its physical page at LSQ "
    "translation finish (VectorChainTable demand_stream_pages). Lets "
    "--stream-demote run without a vector-chain-table prefetcher (policy in "
    "isolation, e.g. --prefetcher none/stride) and broadens "
    "classification from the prefetcher's producer index arrays to "
    "every unit-stride-touched array, including store streams. With a "
    "gdp/viper prefetcher both sources feed the same registry.",
)
parser.add_argument(
    "--stream-demote-second-touch",
    action="store_true",
    help="Second-touch promotion on the demotion policy: a demoted "
    "stream line touched AGAIN while still resident (per-residency "
    "consumed bit) is observed cross-sweep reuse and promotes as "
    "plain LRU instead of being held demoted. Fixes iterative "
    "re-sweep regressions (blackscholes/jacobi/somier) while "
    "preserving single-use stream demotion wins.",
)
parser.add_argument(
    "--stream-demote-page-promote",
    action="store_true",
    help="Second touch also unlearns the whole PAGE: removed from the "
    "stream registry and blocked from re-registration "
    "(VectorChainTable promoted_page_entries FIFO), so new fills of a "
    "proven-reused page insert as plain LRU. Fixes the churn lockout "
    "that per-line second-touch promotion cannot (evicted lines "
    "re-enter demoted and are re-victimized before their second "
    "touch). Use together with --stream-demote-second-touch.",
)
parser.add_argument(
    "--stream-demote-monotone",
    action="store_true",
    help="Monotone-arm gate on the demand-fed stream registry "
    "(VectorChainTable monotone_arm): a unit-stride stream may demote "
    "only after covering arm_distance (4KiB) of VA monotonically, and "
    "the first backward jump beyond backward_slack STICKY-revokes it "
    "and retroactively drops its registered pages — an access below "
    "the stream's high-water mark proves the sweep recurs "
    "(blackscholes NUM_RUNS / jacobi ping-pong / somier timesteps), "
    "so its lines have reuse that demotion would destroy. Requires "
    "--stream-demote-demand (only demand registrations are gated).",
)

args = parser.parse_args()

if args.unified_cache and args.vector_cache:
    print("Error: --unified-cache and --vector-cache are mutually "
          "exclusive (one shared chain vs the split hierarchy)")
    sys.exit(1)
if args.ara_cache and (args.unified_cache or args.vector_cache):
    print("Error: --ara-cache is mutually exclusive with --unified-cache "
          "and --vector-cache (scalar-only L1D + shared L2 is its own "
          "topology)")
    sys.exit(1)
if args.ara_cache and args.stream_demote != "none":
    print("Error: --stream-demote is not supported with --ara-cache yet")
    sys.exit(1)
if args.unified_cache and args.scalar_prefetcher not in (None, "none"):
    print("Error: --scalar-prefetcher needs the split hierarchy's "
          "separate scalar chain; with --unified-cache use --prefetcher")
    sys.exit(1)

l2_prefetcher_active = args.l2_prefetcher not in (None, "none")
if l2_prefetcher_active and args.ara_cache:
    print("Error: --l2-prefetcher is not supported with --ara-cache; the "
          "main --prefetcher already attaches at the shared L2 there")
    sys.exit(1)
if l2_prefetcher_active and not args.unified_cache:
    print("Error: --l2-prefetcher targets the unified L2; it requires "
          "--unified-cache (the split hierarchy uses "
          "--scalar-prefetcher / --prefetcher-side instead)")
    sys.exit(1)
if l2_prefetcher_active and args.prefetcher_level != "l1":
    print("Error: --l2-prefetcher claims the L2; place the main "
          "prefetcher at the L1 (--prefetcher-level l1) or drop one")
    sys.exit(1)
if args.stream_demote != "none" and not (
        args.unified_cache or args.vector_cache or
        args.prefetcher not in (None, "none")):
    print("Error: --stream-demote needs a hierarchy that hosts the "
          "policy (--unified-cache, or the split --vector-cache where "
          "it applies to the VECTOR chain's caches)")
    sys.exit(1)

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

# viper's prefetch_distance is denominated in whole-VLEN chunks; wire the
# hardware VLEN from --vlen so the two knobs cannot drift apart (an explicit
# --pf-param vlen=N still overrides).
if args.prefetcher == "viper" and "vlen" not in pf_params:
    pf_params["vlen"] = str(args.vlen)

scalar_pf_params = {}
for item in args.scalar_pf_param or []:
    if "=" not in item:
        print(f"Error: --scalar-pf-param expects NAME=VALUE, got '{item}'")
        sys.exit(1)
    key, value = item.split("=", 1)
    scalar_pf_params[key] = value

l2_pf_params = {}
for item in args.l2_pf_param or []:
    if "=" not in item:
        print(f"Error: --l2-pf-param expects NAME=VALUE, got '{item}'")
        sys.exit(1)
    key, value = item.split("=", 1)
    l2_pf_params[key] = value
if l2_pf_params and not l2_prefetcher_active:
    print("Error: --l2-pf-param requires --l2-prefetcher (and not 'none')")
    sys.exit(1)

scalar_prefetcher_active = args.scalar_prefetcher not in (None, "none")
if scalar_pf_params and not scalar_prefetcher_active:
    print("Error: --scalar-pf-param requires --scalar-prefetcher "
          "(and not 'none')")
    sys.exit(1)
if scalar_prefetcher_active and prefetcher_active and \
        args.prefetcher_side == "scalar":
    print("Error: --scalar-prefetcher and '--prefetcher-side scalar' both "
          "target the scalar chain; place the main prefetcher on the "
          "vector side or drop one of them")
    sys.exit(1)

scalar_l1d_prefetcher = None
scalar_l2_prefetcher = None
vector_l1d_prefetcher = None
vector_l2_prefetcher = None
prefetcher_mmu = False
prefetcher_tyche_table = False
prefetcher_vector_chain_table = False
prefetcher_revela_table = False
if prefetcher_active or scalar_prefetcher_active or l2_prefetcher_active:
    # The split hierarchy hosts the prefetcher on whichever chain is
    # selected, so it is required — unless the caller asked for the
    # unified shared chain, which hosts prefetchers itself.
    if not args.unified_cache and not args.ara_cache:
        args.vector_cache = True
    from prefetcher_factory import build as build_prefetcher
    from prefetcher_factory import needs_chain_table
    from prefetcher_factory import needs_vector_chain_table
    from prefetcher_factory import needs_mmu as prefetcher_needs_mmu
    from prefetcher_factory import needs_revela_table

if prefetcher_active:
    try:
        factory = build_prefetcher(args.prefetcher, pf_params)
    except ValueError as exc:
        print(f"Error: {exc}")
        sys.exit(1)
    # Virtual-address prefetchers (vimp by default) need the CPU MMU to
    # translate page-crossing prefetch targets.
    prefetcher_mmu = prefetcher_needs_mmu(args.prefetcher, pf_params)
    # Tyche decodes scalar instructions; the vector caches never see
    # scalar accesses in the split hierarchy.
    prefetcher_tyche_table = needs_chain_table(args.prefetcher)
    # The side guards below only apply to the split hierarchy: the
    # unified L1D sees scalar and vector accesses alike, so both the
    # tyche and the gdp/viper sideband channels reach it.
    if prefetcher_tyche_table and not args.unified_cache \
            and not args.ara_cache \
            and args.prefetcher_side != "scalar":
        print(f"Error: --prefetcher {args.prefetcher} requires "
              "--prefetcher-side scalar (it observes scalar loads, "
              "which the VectorSplitter never routes to the vector "
              "caches)")
        sys.exit(1)
    # GDP's CPU-side records are extracted from vector loads; those
    # only reach a vector-side cache in the split hierarchy.
    prefetcher_vector_chain_table = needs_vector_chain_table(args.prefetcher)
    # On --ara-cache the L1D never sees a vector access, so a
    # vector-trained prefetcher at the L1 would simply never train:
    # only the shared L2 can host it.
    if prefetcher_vector_chain_table and args.ara_cache \
            and args.prefetcher_level != "l2":
        print(f"Error: --prefetcher {args.prefetcher} requires "
              "--prefetcher-level l2 with --ara-cache (vector accesses "
              "bypass the scalar-only L1D)")
        sys.exit(1)
    if prefetcher_vector_chain_table and not args.unified_cache \
            and not args.ara_cache \
            and args.prefetcher_side != "vector":
        print(f"Error: --prefetcher {args.prefetcher} requires "
              "--prefetcher-side vector (its CPU-side records come "
              "from vector loads, which the VectorSplitter never "
              "routes to the scalar caches)")
        sys.exit(1)
    # ReVeLA's streams come from unit-stride vector accesses; in the
    # split hierarchy only the vector chain observes them (the drain
    # latches its translation context off demand accesses).
    prefetcher_revela_table = needs_revela_table(args.prefetcher, pf_params)
    if prefetcher_revela_table and args.ara_cache \
            and args.prefetcher_level != "l2":
        print(f"Error: --prefetcher {args.prefetcher} requires "
              "--prefetcher-level l2 with --ara-cache (vector accesses "
              "bypass the scalar-only L1D)")
        sys.exit(1)
    if prefetcher_revela_table and not args.unified_cache \
            and not args.ara_cache \
            and args.prefetcher_side != "vector":
        print(f"Error: --prefetcher {args.prefetcher} requires "
              "--prefetcher-side vector (its streams are unit-stride "
              "vector accesses, which the VectorSplitter never routes "
              "to the scalar caches)")
        sys.exit(1)
    # Route the single factory to one of four caches: {scalar,vector} x {l1,l2}.
    # The unified hierarchy has only one chain; its caches reuse the
    # scalar slots and only --prefetcher-level applies.
    if args.unified_cache or args.ara_cache \
            or args.prefetcher_side == "scalar":
        if args.prefetcher_level == "l1":
            scalar_l1d_prefetcher = factory
        else:
            scalar_l2_prefetcher = factory
    else:
        if args.prefetcher_level == "l1":
            vector_l1d_prefetcher = factory
        else:
            vector_l2_prefetcher = factory

if l2_prefetcher_active:
    # Second, independent prefetcher on the unified L2 — covers the
    # traffic classes an L1-hosted vector prefetcher never learns
    # (scalar ia[] walk, store streams). The guards above ensure the
    # main prefetcher sits at the L1, so the L2 slot is free.
    try:
        l2_factory = build_prefetcher(args.l2_prefetcher, l2_pf_params)
    except ValueError as exc:
        print(f"Error: {exc}")
        sys.exit(1)
    scalar_l2_prefetcher = l2_factory

if args.stream_demote != "none" and not prefetcher_vector_chain_table \
        and not args.stream_demote_demand:
    print("Error: --stream-demote needs a vector-chain-table prefetcher "
          "(--prefetcher gdp or viper) or --stream-demote-demand "
          "to feed the stream-page registry")
    sys.exit(1)

if args.vector_dct_entries < 3:
    # Same bound the C++ constructor enforces (vector_chain_table.cc);
    # caught here so the message names the flag, not the param.
    print("Error: --vector-dct-entries must be >= 3 (head + transform "
          "link + gather)")
    sys.exit(1)

if args.vector_max_transform_stages < 1:
    print("Error: --vector-max-transform-stages must be >= 1")
    sys.exit(1)

if args.stream_demote_monotone and not args.stream_demote_demand:
    print("Error: --stream-demote-monotone gates only demand-side "
          "registrations — it needs --stream-demote-demand (it would "
          "be a silent no-op without it)")
    sys.exit(1)

if scalar_prefetcher_active:
    # Second, independent prefetcher on the scalar L1D (dual configs:
    # e.g. stride on the scalar side next to gdp/vimp on the vector
    # side). The argparse choices already exclude gdp.
    try:
        scalar_factory = build_prefetcher(
            args.scalar_prefetcher, scalar_pf_params
        )
    except ValueError as exc:
        print(f"Error: {exc}")
        sys.exit(1)
    scalar_l1d_prefetcher = scalar_factory
    # The MMU flag is hierarchy-global but harmless on PA prefetchers.
    prefetcher_mmu = prefetcher_mmu or prefetcher_needs_mmu(
        args.scalar_prefetcher, scalar_pf_params
    )
    prefetcher_tyche_table = prefetcher_tyche_table or needs_chain_table(
        args.scalar_prefetcher
    )

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
        prefetcher_needs_chain_table=prefetcher_tyche_table,
        prefetcher_needs_vector_chain_table=prefetcher_vector_chain_table,
        vector_dct_entries=args.vector_dct_entries,
        vector_max_transform_stages=args.vector_max_transform_stages,
        prefetcher_needs_revela_table=prefetcher_revela_table,
        revela_stt_entries=args.revela_stt_entries,
        l1d_mshrs=args.l1d_mshrs,
        l2_mshrs=args.l2_mshrs,
        vector_l1d_mshrs=args.vector_l1d_mshrs,
        vector_l2_mshrs=args.vector_l2_mshrs,
        l1d_tgts_per_mshr=args.l1d_tgts_per_mshr,
        l2_tgts_per_mshr=args.l2_tgts_per_mshr,
        vector_l1d_tgts_per_mshr=args.vector_l1d_tgts_per_mshr,
        vector_l2_tgts_per_mshr=args.vector_l2_tgts_per_mshr,
        stream_demote=args.stream_demote,
        stream_demote_demand=args.stream_demote_demand,
        stream_demote_second_touch=args.stream_demote_second_touch,
        stream_demote_page_promote=args.stream_demote_page_promote,
        stream_demote_monotone=args.stream_demote_monotone,
    )
elif args.ara_cache:
    from ara_cache_hierarchy import AraSharedL2CacheHierarchy

    cache_hierarchy = AraSharedL2CacheHierarchy(
        l1d_size=args.l1d,
        l1i_size="32KiB",
        l2_size=args.l2,
        l1d_prefetcher=scalar_l1d_prefetcher,
        l2_prefetcher=scalar_l2_prefetcher,
        prefetcher_needs_mmu=prefetcher_mmu,
        prefetcher_needs_chain_table=prefetcher_tyche_table,
        prefetcher_needs_vector_chain_table=prefetcher_vector_chain_table,
        vector_dct_entries=args.vector_dct_entries,
        vector_max_transform_stages=args.vector_max_transform_stages,
        prefetcher_needs_revela_table=prefetcher_revela_table,
        revela_stt_entries=args.revela_stt_entries,
        l1d_mshrs=args.l1d_mshrs,
        l2_mshrs=args.l2_mshrs,
        l1d_tgts_per_mshr=args.l1d_tgts_per_mshr,
        l2_tgts_per_mshr=args.l2_tgts_per_mshr,
        vbuf_size=args.ara_vbuf_size,
        vbuf_mshrs=args.ara_vbuf_mshrs,
    )
elif args.unified_cache:
    from unified_cache_hierarchy import UnifiedCacheHierarchy

    cache_hierarchy = UnifiedCacheHierarchy(
        l1d_size=args.l1d,
        l1i_size="32KiB",
        l2_size=args.l2,
        l1d_prefetcher=scalar_l1d_prefetcher,
        l2_prefetcher=scalar_l2_prefetcher,
        prefetcher_needs_mmu=prefetcher_mmu,
        prefetcher_needs_chain_table=prefetcher_tyche_table,
        prefetcher_needs_vector_chain_table=prefetcher_vector_chain_table,
        vector_dct_entries=args.vector_dct_entries,
        vector_max_transform_stages=args.vector_max_transform_stages,
        prefetcher_needs_revela_table=prefetcher_revela_table,
        revela_stt_entries=args.revela_stt_entries,
        l1d_mshrs=args.l1d_mshrs,
        l2_mshrs=args.l2_mshrs,
        l1d_tgts_per_mshr=args.l1d_tgts_per_mshr,
        l2_tgts_per_mshr=args.l2_tgts_per_mshr,
        stream_demote=args.stream_demote,
        stream_demote_demand=args.stream_demote_demand,
        stream_demote_second_touch=args.stream_demote_second_touch,
        stream_demote_page_promote=args.stream_demote_page_promote,
        stream_demote_monotone=args.stream_demote_monotone,
    )
else:
    cache_hierarchy = PrivateL1PrivateL2CacheHierarchy(
        # l1d_size="32KiB", l1i_size="32KiB", l2_size="512KiB"
        l1d_size=args.l1d,
        l1i_size="32KiB",
        l2_size=args.l2,
    )

# memory = SingleChannelDDR3_1600()
if args.mem_type == "perfect":
    from gem5.components.memory.simple import SingleChannelSimpleMemory

    memory = SingleChannelSimpleMemory(
        latency="1ns",
        latency_var="0ns",
        bandwidth="1TiB/s",
        size="8GiB",
    )
else:
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
        print(f"  Prefetcher:       none on the vector side")
    if scalar_prefetcher_active:
        print(f"  Scalar Prefetcher: {args.scalar_prefetcher} on scalar L1D")
        if scalar_pf_params:
            print(f"  Scalar PF Params: {scalar_pf_params}")
elif args.ara_cache:
    print(f"  Vector Caches:    ARA (scalar-only L1D, vector to shared "
          f"L2 via {args.ara_vbuf_size} vbuf, {args.ara_vbuf_mshrs} MSHRs)")
    print(f"  L1D MSHRs:        {args.l1d_mshrs} "
          f"(L2: {args.l2_mshrs})")
    if prefetcher_active:
        cache = "L1D" if args.prefetcher_level == "l1" else "L2"
        print(f"  Prefetcher:       {args.prefetcher} on shared {cache}")
        print(
            f"  PF MMU:           "
            f"{'registered (VA training, page-crossing OK)' if prefetcher_mmu else 'none (PA training, page-crossing dropped)'}"
        )
        if pf_params:
            print(f"  PF Params:        {pf_params}")
    else:
        print(f"  Prefetcher:       none")
    if scalar_prefetcher_active:
        print(f"  Scalar Prefetcher: {args.scalar_prefetcher} on scalar L1D")
        if scalar_pf_params:
            print(f"  Scalar PF Params: {scalar_pf_params}")
elif args.unified_cache:
    print(f"  Vector Caches:    OFF (unified shared L1D/L2, stdlib "
          f"prefetchers stripped)")
    print(f"  L1D MSHRs:        {args.l1d_mshrs} "
          f"(L2: {args.l2_mshrs})")
    if prefetcher_active:
        cache = "L1D" if args.prefetcher_level == "l1" else "L2"
        print(f"  Prefetcher:       {args.prefetcher} on unified {cache}")
        print(
            f"  PF MMU:           "
            f"{'registered (VA training, page-crossing OK)' if prefetcher_mmu else 'none (PA training, page-crossing dropped)'}"
        )
        if pf_params:
            print(f"  PF Params:        {pf_params}")
    else:
        print(f"  Prefetcher:       none")
else:
    print(f"  Vector Caches:    OFF (shared L1D/L2)")
print("-" * 50)
print("Beginning simulation...")
print("=" * 50)

# board.set_se_binary_workload(binary, arguments=[args.parms])
board.set_se_binary_workload(binary, arguments=args.parms.split())

# Guest path identity for non-docker hosts (2026-08-13). When gem5
# runs natively (no docker), the benchmark tree lives under a
# different host prefix -- but every guest-VISIBLE string must stay
# bit-identical to the docker runs: argv strings live on the simulated
# stack, so a different path length shifts the guest memory layout and
# perturbs ROI timing (measured ~3% cycles on the football kernel with
# identical instruction counts). GEM5_GUEST_PATH_MAP=<guest>=<host>
# rewrites argv back to the canonical guest prefix and installs a
# RedirectPath (existing gem5 SE infrastructure, sim/redirect_path.hh)
# so the guest's open() of the canonical prefix resolves to the host
# files. Unset (every docker run), this block is inert.
_path_map = os.environ.get("GEM5_GUEST_PATH_MAP")
if _path_map:
    from m5.objects import RedirectPath

    _guest_prefix, _host_prefix = _path_map.split("=", 1)
    # Syscall-time redirection is a SYSTEM param, not a Process one:
    # Process::checkPathRedirect iterates system->redirectPaths
    # (src/sim/process.cc), and the stdlib board is the System.
    board.redirect_paths = [
        RedirectPath(app_path=_guest_prefix, host_paths=[_host_prefix])
    ]
    for _core in board.get_processor().get_cores():
        _workloads = _core.core.workload
        try:
            _workloads = list(_workloads)
        except TypeError:
            _workloads = [_workloads]
        for _proc in _workloads:
            # executable keeps the host path (the ELF is loaded from
            # disk); only argv is rewritten to the guest-canonical form.
            _proc.cmd = [
                str(c).replace(_host_prefix, _guest_prefix)
                for c in _proc.cmd
            ]

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
