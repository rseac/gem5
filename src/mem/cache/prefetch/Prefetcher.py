# Copyright (c) 2012, 2014, 2019, 2022-2025 Arm Limited
# Copyright (c) 2023 The University of Edinburgh
# All rights reserved.
#
# The license below extends only to copyright in the software and shall
# not be construed as granting a license to any other intellectual
# property including but not limited to intellectual property relating
# to a hardware implementation of the functionality of the software
# licensed hereunder.  You may use the software subject to the license
# terms below provided that you ensure that this notice is replicated
# unmodified and in its entirety in all distributions of the software,
# modified or unmodified, in source code or in binary form.
#
# Copyright (c) 2005 The Regents of The University of Michigan
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

from m5.citations import add_citation
from m5.objects.ClockedObject import ClockedObject
from m5.objects.IndexingPolicies import *
from m5.objects.ReplacementPolicies import *
from m5.objects.Tags import *
from m5.params import *
from m5.proxy import *
from m5.SimObject import *


class HWPProbeEvent:
    def __init__(self, prefetcher, obj, *listOfNames):
        self.obj = obj
        self.prefetcher = prefetcher
        self.names = listOfNames

    def register(self):
        if self.obj:
            for name in self.names:
                self.prefetcher.getCCObject().addEventProbe(
                    self.obj.getCCObject(), name
                )


class BasePrefetcher(ClockedObject):
    type = "BasePrefetcher"
    abstract = True
    cxx_class = "gem5::prefetch::Base"
    cxx_header = "mem/cache/prefetch/base.hh"
    cxx_exports = [PyBindMethod("addEventProbe"), PyBindMethod("addMMU")]
    sys = Param.System(Parent.any, "System this prefetcher belongs to")

    # Get the block size from the parent (system)
    block_size = Param.Int(Parent.cache_line_size, "Block size in bytes")

    on_miss = Param.Bool(False, "Only notify prefetcher on misses")
    on_read = Param.Bool(True, "Notify prefetcher on reads")
    on_write = Param.Bool(True, "Notify prefetcher on writes")
    on_data = Param.Bool(True, "Notify prefetcher on data accesses")
    on_inst = Param.Bool(True, "Notify prefetcher on instruction accesses")
    prefetch_on_access = Param.Bool(
        False,
        "Notify the hardware prefetcher on every access (not just misses)",
    )
    prefetch_on_pf_hit = Param.Bool(
        True,
        "Notify the hardware prefetcher on hit on prefetched lines",
    )
    use_virtual_addresses = Param.Bool(
        False, "Use virtual addresses for prefetching"
    )
    page_bytes = Param.MemorySize(
        "4KiB", "Size of pages for virtual addresses"
    )

    def __init__(self, **kwargs):
        super().__init__(**kwargs)
        self._events = []
        self._mmus = []

    def addEvent(self, newObject):
        self._events.append(newObject)

    # Override the normal SimObject::regProbeListeners method and
    # register deferred event handlers.
    def regProbeListeners(self):
        for mmu in self._mmus:
            self.getCCObject().addMMU(mmu.getCCObject())
        for event in self._events:
            event.register()
        self.getCCObject().regProbeListeners()

    def listenFromProbe(self, simObj, *probeNames):
        if not isinstance(simObj, SimObject):
            raise TypeError("argument must be of SimObject type")
        if len(probeNames) <= 0:
            raise TypeError("probeNames must have at least one element")
        self.addEvent(HWPProbeEvent(self, simObj, *probeNames))

    def registerMMU(self, simObj):
        if not isinstance(simObj, SimObject):
            raise TypeError("argument must be a SimObject type")
        self._mmus.append(simObj)


class MultiPrefetcher(BasePrefetcher):
    type = "MultiPrefetcher"
    cxx_class = "gem5::prefetch::Multi"
    cxx_header = "mem/cache/prefetch/multi.hh"

    prefetchers = VectorParam.BasePrefetcher([], "Array of prefetchers")


class QueuedPrefetcher(BasePrefetcher):
    type = "QueuedPrefetcher"
    abstract = True
    cxx_class = "gem5::prefetch::Queued"
    cxx_header = "mem/cache/prefetch/queued.hh"
    latency = Param.Int(1, "Latency for generated prefetches")
    queue_size = Param.Int(32, "Maximum number of queued prefetches")
    max_prefetch_requests_with_pending_translation = Param.Int(
        32,
        "Maximum number of queued prefetches that have a missing translation",
    )
    queue_squash = Param.Bool(True, "Squash queued prefetch on demand access")
    queue_filter = Param.Bool(True, "Don't queue redundant prefetches")
    cache_snoop = Param.Bool(
        False, "Snoop cache to eliminate redundant request"
    )

    tag_prefetch = Param.Bool(
        True, "Tag prefetch with PC of generating access"
    )

    # The throttle_control_percentage controls how many of the candidate
    # addresses generated by the prefetcher will be finally turned into
    # prefetch requests
    # - If set to 100, all candidates can be discarded (one request
    #   will always be allowed to be generated)
    # - Setting it to 0 will disable the throttle control, so requests are
    #   created for all candidates
    # - If set to 60, 40% of candidates will generate a request, and the
    #   remaining 60% will be generated depending on the current accuracy
    throttle_control_percentage = Param.Percent(
        0,
        "Percentage of requests \
        that can be throttled depending on the accuracy of the prefetcher.",
    )


class StridePrefetcherHashedSetAssociative(TaggedSetAssociative):
    type = "StridePrefetcherHashedSetAssociative"
    cxx_class = "gem5::prefetch::StridePrefetcherHashedSetAssociative"
    cxx_header = "mem/cache/prefetch/stride.hh"


class StridePrefetcher(QueuedPrefetcher):
    type = "StridePrefetcher"
    cxx_class = "gem5::prefetch::Stride"
    cxx_header = "mem/cache/prefetch/stride.hh"

    # Do not consult stride prefetcher on instruction accesses
    on_inst = False

    confidence_counter_bits = Param.Unsigned(
        3, "Number of bits of the confidence counter"
    )
    initial_confidence = Param.Unsigned(
        4, "Starting confidence of new entries"
    )
    confidence_threshold = Param.Percent(
        50, "Prefetch generation confidence threshold"
    )

    use_requestor_id = Param.Bool(True, "Use requestor id based history")

    use_cache_line_address = Param.Bool(
        True,
        "If this parameter is set to True, then the prefetcher will "
        "operate on cache line addresses, else it would operate on word "
        "addresses",
    )

    degree = Param.Int(4, "Number of prefetches to generate")
    distance = Param.Unsigned(
        0,
        "How far ahead of the demand stream to start prefetching. "
        "Skip this number of strides ahead of the first identified prefetch, "
        "then generate `degree` prefetches at `stride` intervals. "
        "A value of zero indicates no skip.",
    )

    table_assoc = Param.Int(4, "Associativity of the PC table")
    table_entries = Param.MemorySize("64", "Number of entries of the PC table")
    table_indexing_policy = Param.TaggedIndexingPolicy(
        StridePrefetcherHashedSetAssociative(
            entry_size=1, assoc=Parent.table_assoc, size=Parent.table_entries
        ),
        "Indexing policy of the PC table",
    )
    table_replacement_policy = Param.BaseReplacementPolicy(
        RandomRP(), "Replacement policy of the PC table"
    )


class TaggedPrefetcher(QueuedPrefetcher):
    type = "TaggedPrefetcher"
    cxx_class = "gem5::prefetch::Tagged"
    cxx_header = "mem/cache/prefetch/tagged.hh"

    degree = Param.Int(2, "Number of prefetches to generate")


class IndirectMemoryPrefetcher(QueuedPrefetcher):
    type = "IndirectMemoryPrefetcher"
    cxx_class = "gem5::prefetch::IndirectMemory"
    cxx_header = "mem/cache/prefetch/indirect_memory.hh"
    pt_table_entries = Param.MemorySize(
        "16", "Number of entries of the Prefetch Table"
    )
    pt_table_assoc = Param.Unsigned(16, "Associativity of the Prefetch Table")
    pt_table_indexing_policy = Param.TaggedIndexingPolicy(
        TaggedSetAssociative(
            entry_size=1,
            assoc=Parent.pt_table_assoc,
            size=Parent.pt_table_entries,
        ),
        "Indexing policy of the pattern table",
    )
    pt_table_replacement_policy = Param.BaseReplacementPolicy(
        LRURP(), "Replacement policy of the pattern table"
    )
    max_prefetch_distance = Param.Unsigned(16, "Maximum prefetch distance")
    num_indirect_counter_bits = Param.Unsigned(
        3, "Number of bits of the indirect counter"
    )
    ipd_table_entries = Param.MemorySize(
        "4", "Number of entries of the Indirect Pattern Detector"
    )
    ipd_table_assoc = Param.Unsigned(
        4, "Associativity of the Indirect Pattern Detector"
    )
    ipd_table_indexing_policy = Param.TaggedIndexingPolicy(
        TaggedSetAssociative(
            entry_size=1,
            assoc=Parent.ipd_table_assoc,
            size=Parent.ipd_table_entries,
        ),
        "Indexing policy of the Indirect Pattern Detector",
    )
    ipd_table_replacement_policy = Param.BaseReplacementPolicy(
        LRURP(), "Replacement policy of the Indirect Pattern Detector"
    )
    shift_values = VectorParam.Int([2, 3, 4, -3], "Shift values to evaluate")
    addr_array_len = Param.Unsigned(4, "Number of misses tracked")
    prefetch_threshold = Param.Unsigned(
        2, "Counter threshold to start the indirect prefetching"
    )
    stream_counter_threshold = Param.Unsigned(
        4, "Counter threshold to enable the stream prefetcher"
    )
    streaming_distance = Param.Unsigned(
        4, "Number of prefetches to generate when using the stream prefetcher"
    )


class VectorIndirectMemoryPrefetcher(QueuedPrefetcher):
    """Indirect memory prefetcher for vectorized (chunked) index streams.

    Generalizes IndirectMemoryPrefetcher to index arrays walked one vector
    register at a time (e.g. RVV unit-stride loads feeding vluxei gathers):
    streams are detected at chunk granularity (next address == previous
    address + previous request size), every chunk payload is sliced into
    index_size-byte elements, and one indirect prefetch is generated per
    index of the chunk. See mem/cache/prefetch/vector_indirect_memory.hh.
    """

    type = "VectorIndirectMemoryPrefetcher"
    cxx_class = "gem5::prefetch::VectorIndirectMemory"
    cxx_header = "mem/cache/prefetch/vector_indirect_memory.hh"

    # Vector index loads are data reads; ignore instruction accesses.
    on_inst = False
    # Chunk payloads are only readable on hits, and in steady state the
    # index loads hit on lines this prefetcher itself streamed in, so all
    # accesses must be observed rather than only misses.
    prefetch_on_access = True
    # Indirect targets are scattered across pages; train on virtual
    # addresses and translate the targets through the registered MMU
    # (call registerMMU() on this object in the config script).
    use_virtual_addresses = True
    # One chunk can legally generate tens of candidates (targets + stream).
    queue_size = 64

    pt_table_entries = Param.MemorySize(
        "16", "Number of entries of the Prefetch Table"
    )
    pt_table_assoc = Param.Unsigned(16, "Associativity of the Prefetch Table")
    pt_table_indexing_policy = Param.TaggedIndexingPolicy(
        TaggedSetAssociative(
            entry_size=1,
            assoc=Parent.pt_table_assoc,
            size=Parent.pt_table_entries,
        ),
        "Indexing policy of the pattern table",
    )
    pt_table_replacement_policy = Param.BaseReplacementPolicy(
        LRURP(), "Replacement policy of the pattern table"
    )
    num_indirect_counter_bits = Param.Unsigned(
        3, "Number of bits of the indirect counter"
    )
    ipd_table_entries = Param.MemorySize(
        "4", "Number of entries of the Indirect Pattern Detector"
    )
    ipd_table_assoc = Param.Unsigned(
        4, "Associativity of the Indirect Pattern Detector"
    )
    ipd_table_indexing_policy = Param.TaggedIndexingPolicy(
        TaggedSetAssociative(
            entry_size=1,
            assoc=Parent.ipd_table_assoc,
            size=Parent.ipd_table_entries,
        ),
        "Indexing policy of the Indirect Pattern Detector",
    )
    ipd_table_replacement_policy = Param.BaseReplacementPolicy(
        LRURP(), "Replacement policy of the Indirect Pattern Detector"
    )
    shift_values = VectorParam.Int(
        [0, 1, 2, 3, 4], "Shift values to evaluate"
    )
    addr_array_len = Param.Unsigned(
        4, "Recent tracked misses kept per IPD entry for pair matching"
    )
    ipd_chunk_history = Param.Unsigned(
        16,
        "Recent chunks whose leading indices an IPD entry keeps; must "
        "cover how far the OoO core runs index reads ahead of the "
        "corresponding gather misses",
    )
    confidence_chunk_history = Param.Unsigned(
        16,
        "Recent chunks whose indices an enabled entry keeps for "
        "confidence matching; must cover the same index-read-to-miss "
        "lag as ipd_chunk_history, or confirming accesses only ever "
        "get checked against newer, disjoint chunks and confidence "
        "never builds",
    )
    demotion_chunks = Param.Unsigned(
        16,
        "Consecutive zero-confidence chunks before an enabled entry is "
        "demoted and may retrain (0 disables demotion)",
    )
    prefetch_threshold = Param.Unsigned(
        2, "Counter threshold to start the indirect prefetching"
    )
    stream_counter_threshold = Param.Unsigned(
        4, "Counter threshold to enable the chunk-stream prefetcher"
    )
    streaming_distance = Param.Unsigned(
        4, "Number of chunks prefetched ahead in the index array"
    )
    stream_dedup = Param.Bool(
        True,
        "Emit each stream-prefetch line only once per walk (per-entry "
        "high-water mark, reset on discontinuity). The streaming window "
        "advances one chunk per access but spans streaming_distance "
        "chunks, so without this every access re-emits an almost "
        "identical window; the duplicates are dropped by the queue or "
        "issue anyway and count as pfLate in-cache hits.",
    )
    index_size = Param.Unsigned(
        4, "Size in bytes of one index element inside a chunk (EEW/8)"
    )
    index_signed = Param.Bool(
        True, "Sign-extend index elements (else zero-extend)"
    )
    max_indices_per_chunk = Param.Unsigned(
        64, "Maximum number of index elements sliced out of one chunk"
    )
    ipd_indices_per_chunk = Param.Unsigned(
        8, "Number of leading chunk indices used for IPD correlation"
    )
    max_indirect_targets = Param.Unsigned(
        32, "Maximum indirect prefetches generated per chunk access"
    )
    ipd_train_on_hits = Param.Bool(
        False,
        "Correlate IPD candidates on observed hits too, not only misses "
        "(useful when the target array mostly hits the attached cache)",
    )
    indirect_delta = Param.Unsigned(
        0,
        "Lookahead distance, in chunks, for indirect target prefetches. "
        "0 issues the current chunk's targets from its own payload. N>0 "
        "captures the index lines returned by this prefetcher's own "
        "index-array prefetches when they fill the cache, and issues "
        "their targets once the demand stream is within N chunks. The "
        "effective lookahead is bounded by streaming_distance (index "
        "lines only fill that far ahead).",
    )
    pending_fill_entries = Param.Unsigned(
        32,
        "Expected-fill table entries: index lines awaiting capture "
        "(delta mode)",
    )
    pending_index_sets = Param.Unsigned(
        8,
        "Captured index-line sets buffered per stream entry (delta mode)",
    )


class GDPPrefetcher(QueuedPrefetcher):
    """Gather Dataflow Prefetcher (GDP): prefetches A[f(B[i])] vector
    gathers with no training — the pattern is extracted
    architecturally. The CPU side (cpu/gdp_table.hh) records the
    transform chain between a unit-stride producer load and its gather
    (DCT) and links them in one iteration (backward propagation at the
    gather's dispatch); this prefetcher streams the index array ahead
    architecturally, captures index-line fills by range membership in
    the stream window, and replays each element through the recorded
    transform ops + a dedicated base adder — so any A[f(B[i])] the
    chain expresses is prefetched exactly. No confidence, no kill
    switch, no training. Vector-side design (requires
    --prefetcher-side vector). Wire ONE GdpChainTable per core to
    both this prefetcher's link_table and the CPU's gdp_table.
    See mem/cache/prefetch/gdp.hh."""

    type = "GDPPrefetcher"
    cxx_class = "gem5::prefetch::GDP"
    cxx_header = "mem/cache/prefetch/gdp.hh"

    # Index loads are data reads; ignore instruction accesses.
    on_inst = False
    # The stream walk needs every chunk event, hit or miss.
    prefetch_on_access = True
    # Chain values are virtual; train on VAs and translate targets
    # through the registered MMU.
    use_virtual_addresses = True
    queue_size = 64

    link_table = Param.GdpChainTable(
        NULL,
        "CPU-side chain/link table (must be the same instance as the "
        "CPU's gdp_table param)",
    )
    streaming_distance = Param.Unsigned(
        8,
        "Index-array stream lookahead in lines. Also sets the indirect "
        "lookahead (captures come from the stream's fills): full "
        "indirect timeliness needs distance*chunk-cadence to cover TWO "
        "memory round-trips (index line + target line), so expect "
        "larger values than a plain stream prefetcher wants.",
    )
    stream_only = Param.Bool(
        False,
        "Ablation: stream the index arrays but never capture/replay "
        "(isolates the architectural-stream contribution)",
    )
    slice_buffer_entries = Param.Unsigned(
        2,
        "Line entries in the slice buffer in front of each producer "
        "replay pipeline: captured line payloads held until fully "
        "replayed (or stale-aborted); a fill arriving with the buffer "
        "full is dropped (bufferBusyDrops)",
    )
    pipelines = Param.Unsigned(
        2, "Concurrently configured producer replay pipelines"
    )
    pipelines_per_gather = Param.Unsigned(
        0,
        "Replay pipelines behind ONE gather: how many indices of a "
        "captured index line are replayed per drain event, one per "
        "pipeline. Each replicates the transform-op chain + base "
        "adder, all fed from the same slice-buffer entry, so N "
        "consecutive elements convert together instead of streaming "
        "through a single 1-elem/event pipe. Filtered and "
        "line-deduplicated elements still occupy a pipeline (they "
        "went through the datapath); only emitted targets consume "
        "prefetch-queue slots. Width is per gather -- exhausting one "
        "producer's pipelines does not stall another's, unlike the "
        "shared queue budget. 0 = unbounded, the legacy model in "
        "which replay width never binds (keeps pre-2026-07-29 "
        "results reproducible).",
    )
    routing_entries = Param.Unsigned(
        32, "Index Routing Table (IRT) capacity: index lines "
        "registered for capture; a matching fill is routed to the "
        "owning producer's slice buffer"
    )


class VectorTychePrefetcher(QueuedPrefetcher):
    """VTyche (Vector Tyche): an A[B[i]] gather prefetcher — GDP's
    architectural discovery with IMP's linear equation for generation.

    Shares GDP's CPU-side GdpChainTable (producer identification,
    producer->gather link, base snoop) and GDP's runtime skeleton
    (architectural index-array stream, range-based fill capture), but
    collapses the recorded transform chain ONCE, at adoption, into
    ``target = base + (index << shift)`` instead of latching a replay
    pipeline. Fills latch the raw payload into a slice buffer exactly
    as GDP does; at the drain, ONE whole line per event converts
    through the parallel lane array — one shift-and-add lane per
    sliced index, blkSize/EEW lanes — into an output latch that
    releases into queue room. The drain's compute width (whole line
    vs GDP's pipelines_per_gather elements) is the entire structural
    difference between the two designs.

    The collapse accepts at most one leading vsext/vzext, any number of
    vsll and power-of-2 vmul (folded into ``shift``), and any number of
    scalar vadd/vsub (folded into ``base``). Chains needing anything
    else — a general multiply, a right shift, a reversed subtract, a
    logic op — are rejected outright (chainsRejected), so the gap
    against GDP on the same kernel is exactly the transform pipeline's
    contribution.

    Vector-side design (requires --prefetcher-side vector). Wire ONE
    GdpChainTable per core to both this prefetcher's link_table and the
    CPU's gdp_table. See mem/cache/prefetch/vector_tyche.hh."""

    type = "VectorTychePrefetcher"
    cxx_class = "gem5::prefetch::VectorTyche"
    cxx_header = "mem/cache/prefetch/vector_tyche.hh"

    # Index loads are data reads; ignore instruction accesses.
    on_inst = False
    # The stream walk needs every chunk event, hit or miss.
    prefetch_on_access = True
    # Index values are virtual; train on VAs and translate targets
    # through the registered MMU.
    use_virtual_addresses = True
    queue_size = 64

    link_table = Param.GdpChainTable(
        NULL,
        "CPU-side chain/link table (must be the same instance as the "
        "CPU's gdp_table param). Shared with GDP: the discovery half "
        "of the two designs is identical.",
    )
    streaming_distance = Param.Unsigned(
        8,
        "Index-array stream lookahead in lines. Also sets the indirect "
        "lookahead (captures come from the stream's fills): full "
        "indirect timeliness needs distance*chunk-cadence to cover TWO "
        "memory round-trips (index line + target line).",
    )
    stream_only = Param.Bool(
        False,
        "Ablation: stream the index arrays but never capture/convert "
        "(isolates the architectural-stream contribution)",
    )
    slice_buffer_entries = Param.Unsigned(
        2,
        "Captured raw index lines each producer's slice buffer holds "
        "(GDP's slice_buffer_entries, same name by design: the "
        "staging is identical, only the drain's compute engine "
        "differs). A fill arriving with the buffer full is dropped "
        "whole (bufferBusyDrops).",
    )
    pipelines = Param.Unsigned(
        2, "Concurrently configured producer pipelines"
    )
    routing_entries = Param.Unsigned(
        32,
        "Index Routing Table (IRT) capacity: index lines registered "
        "for capture; a matching fill is routed to the owning "
        "producer's conversion array",
    )
    drain_floor = Param.Int(
        8,
        "Minimum targets emitted per access event when the prefetch "
        "queue is full (bounded displacement). Models the continuous "
        "queue drain of real hardware, which gem5's pull-only queue "
        "lacks: at 0 the emission wedges precisely when miss pressure "
        "is highest. Set 0 for a pure stall-on-full.",
    )
    stream_start_at_distance = Param.Bool(
        False,
        "On a fresh or reset stream window (cold start, backward-jump "
        "restart), jump the stream walk straight to the frontier at "
        "streaming_distance instead of burst-filling the whole "
        "[next-chunk, distance] window. The skipped near-window lines "
        "are left to demand misses (late-but-coalescing prefetches for "
        "them are not issued), buying the frontier lines immediate "
        "issue slots; their chunks' gather targets are fed through the "
        "demand-miss capture path instead of the prefetch-window path. "
        "Steady-state behavior is identical either way (the limitAddr "
        "high-water mark already makes ongoing emission frontier-only).",
    )
    drain_period = Param.Cycles(
        1,
        "Self-clocked drain period. While conversion work is buffered "
        "(slice buffers or output latches non-empty), the drain fires "
        "on its own event every this-many cycles instead of waiting "
        "for the next demand access — so buffered index lines keep "
        "converting and emitting through core stalls and bursts. The "
        "stream walk stays demand-anchored (the window only advances "
        "with demand), so this moves emission earlier WITHIN the "
        "window, never past it. The self-clocked path waits for real "
        "queue room rather than forcing drain_floor. 0 restores the "
        "legacy demand-clocked drain (pre-Aug-2026 behavior).",
    )
    dedup_buffer_size = Param.Unsigned(
        0,
        "Cross-line dedup window, in index lines (0 = disabled). "
        "Every target line is checked against the lines emitted by "
        "that producer's last N processed index lines; matches are "
        "dropped (targetsCrossDeduplicated) instead of re-entering "
        "the prefetch queue. The one-line-per-event conversion "
        "already serializes lines, so N sets only the window depth. "
        "Closes the redundancy the per-line dedup cannot see: "
        "neighboring index lines re-targeting the same data lines.",
    )
    row_schedule_bits = Param.Unsigned(
        0,
        "DRAM row-aware issue order (0 = disabled, FIFO). When set, "
        "getPacket promotes the oldest queued prefetch whose physical "
        "address shares a DRAM row with the last-issued prefetch "
        "(row = paddr >> row_schedule_bits) to the head of its "
        "priority group, so row-mates issue back-to-back and amortize "
        "one ACT/PRE pair instead of paying tRP+tRCD each. Only "
        "already-due, equal-priority entries are considered, so the "
        "queue's priority order and ready-time bookkeeping are "
        "preserved. Set to 13 for the 8KiB row buffer of "
        "DDR4_2400_8x8 (8 devices x 1KiB) under the default "
        "RoRaBaCoCh mapping, where column bits occupy paddr[12:6] so "
        "one DRAM row is an 8KiB-aligned physical region.",
    )


class VectorTyche2Prefetcher(QueuedPrefetcher):
    """VTyche2: VTyche with DECOUPLED metadata/data lead.\n\n    Deep index staging (index_distance) + cursor-scheduled\n    near release (release_distance): index lines are fetched\n    and captured far ahead (cheap SRAM staging, L2 residency),\n    while converted targets and just-in-time L1 promotions are\n    released only when the walk cursor closes to within\n    release_distance chunks. Separates the two failure modes a\n    single distance couples: late targets (fix with deep\n    index_distance) vs evicted-before-use targets (fix with\n    shallow release_distance).\n\n    Base design: VTyche, an A[B[i]] gather prefetcher — GDP's
    architectural discovery with IMP's linear equation for generation.

    Shares GDP's CPU-side GdpChainTable (producer identification,
    producer->gather link, base snoop) and GDP's runtime skeleton
    (architectural index-array stream, range-based fill capture), but
    collapses the recorded transform chain ONCE, at adoption, into
    ``target = base + (index << shift)`` instead of latching a replay
    pipeline. Fills latch the raw payload into a slice buffer exactly
    as GDP does; at the drain, ONE whole line per event converts
    through the parallel lane array — one shift-and-add lane per
    sliced index, blkSize/EEW lanes — into an output latch that
    releases into queue room. The drain's compute width (whole line
    vs GDP's pipelines_per_gather elements) is the entire structural
    difference between the two designs.

    The collapse accepts at most one leading vsext/vzext, any number of
    vsll and power-of-2 vmul (folded into ``shift``), and any number of
    scalar vadd/vsub (folded into ``base``). Chains needing anything
    else — a general multiply, a right shift, a reversed subtract, a
    logic op — are rejected outright (chainsRejected), so the gap
    against GDP on the same kernel is exactly the transform pipeline's
    contribution.

    Vector-side design (requires --prefetcher-side vector). Wire ONE
    GdpChainTable per core to both this prefetcher's link_table and the
    CPU's gdp_table. See mem/cache/prefetch/vector_tyche.hh."""

    type = "VectorTyche2Prefetcher"
    cxx_class = "gem5::prefetch::VectorTyche2"
    cxx_header = "mem/cache/prefetch/vector_tyche2.hh"

    # Index loads are data reads; ignore instruction accesses.
    on_inst = False
    # The stream walk needs every chunk event, hit or miss.
    prefetch_on_access = True
    # Index values are virtual; train on VAs and translate targets
    # through the registered MMU.
    use_virtual_addresses = True
    queue_size = 64

    link_table = Param.GdpChainTable(
        NULL,
        "CPU-side chain/link table (must be the same instance as the "
        "CPU's gdp_table param). Shared with GDP: the discovery half "
        "of the two designs is identical.",
    )
    index_distance = Param.Unsigned(
        32,
        "DEEP staging distance in chunks: how far ahead of the walk "
        "cursor index/stream lines are fetched (staging in the L2) and "
        "their payloads captured into the slice buffers. Unlike v1's "
        "streaming_distance this does NOT set when targets are "
        "fetched -- release_distance does -- so it can be large "
        "without target-residency cost.",
    )
    release_distance = Param.Unsigned(
        3,
        "NEAR release distance in chunks: a captured index line "
        "converts and its targets issue only once the cursor is "
        "within this many chunks; the line itself is re-promoted "
        "into the L1 on the same schedule. Must cover the target "
        "fetch latency (release_distance * chunk-cadence > one "
        "L2/DRAM round trip) and stay well inside the L1 survival "
        "horizon. Must be < index_distance.",
    )
    release_pace = Param.Unsigned(
        0,
        "Max target addresses emitted per drain firing (0 = "
        "unlimited). Spreads a due batch across firings instead of "
        "dumping ~a chunk's worth of targets at the release-gate "
        "edge: with drain_period=1, release_pace=2 spreads ~28 "
        "targets over >=14 cycles, smoothing queue/MSHR contention "
        "at the cost of <15%% of the release margin for the batch's "
        "last target.",
    )
    stream_only = Param.Bool(
        False,
        "Ablation: stream the index arrays but never capture/convert "
        "(isolates the architectural-stream contribution)",
    )
    slice_buffer_entries = Param.Unsigned(
        128,
        "Captured raw index lines each producer's slice buffer holds "
        "(GDP's slice_buffer_entries, same name by design: the "
        "staging is identical, only the drain's compute engine "
        "differs). A fill arriving with the buffer full is dropped "
        "whole (bufferBusyDrops).",
    )
    pipelines = Param.Unsigned(
        2, "Concurrently configured producer pipelines"
    )
    routing_entries = Param.Unsigned(
        128,
        "Index Routing Table (IRT) capacity: index lines registered "
        "for capture; a matching fill is routed to the owning "
        "producer's conversion array",
    )
    drain_floor = Param.Int(
        8,
        "Minimum targets emitted per access event when the prefetch "
        "queue is full (bounded displacement). Models the continuous "
        "queue drain of real hardware, which gem5's pull-only queue "
        "lacks: at 0 the emission wedges precisely when miss pressure "
        "is highest. Set 0 for a pure stall-on-full.",
    )
    stream_start_at_distance = Param.Bool(
        False,
        "On a fresh or reset stream window (cold start, backward-jump "
        "restart), jump the stream walk straight to the frontier at "
        "streaming_distance instead of burst-filling the whole "
        "[next-chunk, distance] window. The skipped near-window lines "
        "are left to demand misses (late-but-coalescing prefetches for "
        "them are not issued), buying the frontier lines immediate "
        "issue slots; their chunks' gather targets are fed through the "
        "demand-miss capture path instead of the prefetch-window path. "
        "Steady-state behavior is identical either way (the limitAddr "
        "high-water mark already makes ongoing emission frontier-only).",
    )
    drain_period = Param.Cycles(
        1,
        "Self-clocked drain period. While conversion work is buffered "
        "(slice buffers or output latches non-empty), the drain fires "
        "on its own event every this-many cycles instead of waiting "
        "for the next demand access — so buffered index lines keep "
        "converting and emitting through core stalls and bursts. The "
        "stream walk stays demand-anchored (the window only advances "
        "with demand), so this moves emission earlier WITHIN the "
        "window, never past it. The self-clocked path waits for real "
        "queue room rather than forcing drain_floor. 0 restores the "
        "legacy demand-clocked drain (pre-Aug-2026 behavior).",
    )
    dedup_buffer_size = Param.Unsigned(
        0,
        "Cross-line dedup window, in index lines (0 = disabled). "
        "Every target line is checked against the lines emitted by "
        "that producer's last N processed index lines; matches are "
        "dropped (targetsCrossDeduplicated) instead of re-entering "
        "the prefetch queue. The one-line-per-event conversion "
        "already serializes lines, so N sets only the window depth. "
        "Closes the redundancy the per-line dedup cannot see: "
        "neighboring index lines re-targeting the same data lines.",
    )



class TychePrefetcher(QueuedPrefetcher):
    """Dependency-chain indirect prefetcher, a gem5/RISC-V port of the
    Tyche artifact (ChampSim/LoongArch). The CPU side
    (cpu/tyche_table.hh) roots chains at IP-stride loads and records the
    instructions between them and dependent loads (op + trained constant
    operand); this prefetcher stride-prefetches each confident head
    load, captures the fill, replays the recorded ALU ops on the loaded
    value and prefetches the dependent loads' addresses — any A[f(B[i])]
    the chain ALU can express. Scalar-side design: it decodes scalar
    RV64 instructions, so attach it to the scalar L1D (--scalar-
    prefetcher tyche). Wire ONE TycheChainTable per core to both this
    prefetcher's chain_table and the CPU's tyche_table.
    See mem/cache/prefetch/tyche.hh."""

    type = "TychePrefetcher"
    cxx_class = "gem5::prefetch::Tyche"
    cxx_header = "mem/cache/prefetch/tyche.hh"

    # Loads are data reads; ignore instruction accesses.
    on_inst = False
    # The IPT must be trained on every demand access, hit or miss.
    prefetch_on_access = True
    # Chain values are pointers/indices in virtual space; train on
    # virtual addresses and translate the targets through the registered
    # MMU (call registerMMU() on this object in the config script).
    use_virtual_addresses = True
    queue_size = 64

    chain_table = Param.TycheChainTable(
        NULL,
        "CPU-side chain table (must be the same instance as the CPU's "
        "tyche_table param)",
    )
    stride_distance = Param.Unsigned(
        32,
        "Head-load stride lookahead in iterations (artifact "
        "L1_STRIDE_DISTANCE)",
    )
    stride_only = Param.Bool(
        False,
        "Disable the chain walk, keeping only the IP-stride prefetches "
        "(the artifact's only_stride ablation)",
    )
    walk_entries = Param.Unsigned(
        16, "In-flight walk-step capacity (artifact AGQ_SIZE)"
    )
    successors_per_wakeup = Param.Unsigned(
        4,
        "Chain successors woken per returned value (artifact "
        "ISQ_WRITE_PORT)",
    )
    max_chain_hops = Param.Unsigned(
        16, "Total ALU replays allowed per captured fill (cycle guard)"
    )
    pending_target_entries = Param.Unsigned(
        32, "Buffered fill-computed chain target capacity"
    )


class SignaturePathPrefetcher(QueuedPrefetcher):
    type = "SignaturePathPrefetcher"
    cxx_class = "gem5::prefetch::SignaturePath"
    cxx_header = "mem/cache/prefetch/signature_path.hh"

    signature_shift = Param.UInt8(
        3, "Number of bits to shift when calculating a new signature"
    )
    signature_bits = Param.UInt16(12, "Size of the signature, in bits")
    signature_table_entries = Param.MemorySize(
        "1024", "Number of entries of the signature table"
    )
    signature_table_assoc = Param.Unsigned(
        2, "Associativity of the signature table"
    )
    signature_table_indexing_policy = Param.TaggedIndexingPolicy(
        TaggedSetAssociative(
            entry_size=1,
            assoc=Parent.signature_table_assoc,
            size=Parent.signature_table_entries,
        ),
        "Indexing policy of the signature table",
    )
    signature_table_replacement_policy = Param.BaseReplacementPolicy(
        LRURP(), "Replacement policy of the signature table"
    )

    num_counter_bits = Param.UInt8(
        3, "Number of bits of the saturating counters"
    )
    pattern_table_entries = Param.MemorySize(
        "4096", "Number of entries of the pattern table"
    )
    pattern_table_assoc = Param.Unsigned(
        1, "Associativity of the pattern table"
    )
    strides_per_pattern_entry = Param.Unsigned(
        4, "Number of strides stored in each pattern entry"
    )
    pattern_table_indexing_policy = Param.TaggedIndexingPolicy(
        TaggedSetAssociative(
            entry_size=1,
            assoc=Parent.pattern_table_assoc,
            size=Parent.pattern_table_entries,
        ),
        "Indexing policy of the pattern table",
    )
    pattern_table_replacement_policy = Param.BaseReplacementPolicy(
        LRURP(), "Replacement policy of the pattern table"
    )

    prefetch_confidence_threshold = Param.Float(
        0.5, "Minimum confidence to issue prefetches"
    )
    lookahead_confidence_threshold = Param.Float(
        0.75, "Minimum confidence to continue exploring lookahead entries"
    )


class SignaturePathPrefetcherV2(SignaturePathPrefetcher):
    type = "SignaturePathPrefetcherV2"
    cxx_class = "gem5::prefetch::SignaturePathV2"
    cxx_header = "mem/cache/prefetch/signature_path_v2.hh"

    signature_table_entries = "256"
    signature_table_assoc = 1
    pattern_table_entries = "512"
    pattern_table_assoc = 1
    num_counter_bits = 4
    prefetch_confidence_threshold = 0.25
    lookahead_confidence_threshold = 0.25

    global_history_register_entries = Param.MemorySize(
        "8", "Number of entries of global history register"
    )
    global_history_register_indexing_policy = Param.TaggedIndexingPolicy(
        TaggedSetAssociative(
            entry_size=1,
            assoc=Parent.global_history_register_entries,
            size=Parent.global_history_register_entries,
        ),
        "Indexing policy of the global history register",
    )
    global_history_register_replacement_policy = Param.BaseReplacementPolicy(
        LRURP(), "Replacement policy of the global history register"
    )


class AccessMapPatternMatching(ClockedObject):
    type = "AccessMapPatternMatching"
    cxx_class = "gem5::prefetch::AccessMapPatternMatching"
    cxx_header = "mem/cache/prefetch/access_map_pattern_matching.hh"

    block_size = Param.Unsigned(
        Parent.block_size,
        "Cacheline size used by the prefetcher using this object",
    )

    limit_stride = Param.Unsigned(
        0, "Limit the strides checked up to -X/X, if 0, disable the limit"
    )
    start_degree = Param.Unsigned(
        4, "Initial degree (Maximum number of prefetches generated"
    )
    hot_zone_size = Param.MemorySize("2KiB", "Memory covered by a hot zone")
    access_map_table_entries = Param.MemorySize(
        "256", "Number of entries in the access map table"
    )
    access_map_table_assoc = Param.Unsigned(
        8, "Associativity of the access map table"
    )
    access_map_table_indexing_policy = Param.TaggedIndexingPolicy(
        TaggedSetAssociative(
            entry_size=1,
            assoc=Parent.access_map_table_assoc,
            size=Parent.access_map_table_entries,
        ),
        "Indexing policy of the access map table",
    )
    access_map_table_replacement_policy = Param.BaseReplacementPolicy(
        LRURP(), "Replacement policy of the access map table"
    )
    high_coverage_threshold = Param.Float(
        0.25, "A prefetch coverage factor bigger than this is considered high"
    )
    low_coverage_threshold = Param.Float(
        0.125, "A prefetch coverage factor smaller than this is considered low"
    )
    high_accuracy_threshold = Param.Float(
        0.5, "A prefetch accuracy factor bigger than this is considered high"
    )
    low_accuracy_threshold = Param.Float(
        0.25, "A prefetch accuracy factor smaller than this is considered low"
    )
    high_cache_hit_threshold = Param.Float(
        0.875, "A cache hit ratio bigger than this is considered high"
    )
    low_cache_hit_threshold = Param.Float(
        0.75, "A cache hit ratio smaller than this is considered low"
    )
    epoch_cycles = Param.Cycles(256000, "Cycles in an epoch period")
    offchip_memory_latency = Param.Latency(
        "30ns", "Memory latency used to compute the required memory bandwidth"
    )


class AMPMPrefetcher(QueuedPrefetcher):
    type = "AMPMPrefetcher"
    cxx_class = "gem5::prefetch::AMPM"
    cxx_header = "mem/cache/prefetch/access_map_pattern_matching.hh"
    ampm = Param.AccessMapPatternMatching(
        AccessMapPatternMatching(), "Access Map Pattern Matching object"
    )


class DeltaCorrelatingPredictionTables(SimObject):
    type = "DeltaCorrelatingPredictionTables"
    cxx_class = "gem5::prefetch::DeltaCorrelatingPredictionTables"
    cxx_header = "mem/cache/prefetch/delta_correlating_prediction_tables.hh"
    deltas_per_entry = Param.Unsigned(
        20, "Number of deltas stored in each table entry"
    )
    delta_bits = Param.Unsigned(12, "Bits per delta")
    delta_mask_bits = Param.Unsigned(
        8, "Lower bits to mask when comparing deltas"
    )
    table_entries = Param.MemorySize("128", "Number of entries in the table")
    table_assoc = Param.Unsigned(128, "Associativity of the table")
    table_indexing_policy = Param.BaseIndexingPolicy(
        SetAssociative(
            entry_size=1, assoc=Parent.table_assoc, size=Parent.table_entries
        ),
        "Indexing policy of the table",
    )
    table_replacement_policy = Param.BaseReplacementPolicy(
        LRURP(), "Replacement policy of the table"
    )


class DCPTPrefetcher(QueuedPrefetcher):
    type = "DCPTPrefetcher"
    cxx_class = "gem5::prefetch::DCPT"
    cxx_header = "mem/cache/prefetch/delta_correlating_prediction_tables.hh"
    dcpt = Param.DeltaCorrelatingPredictionTables(
        DeltaCorrelatingPredictionTables(),
        "Delta Correlating Prediction Tables object",
    )


class IrregularStreamBufferPrefetcher(QueuedPrefetcher):
    type = "IrregularStreamBufferPrefetcher"
    cxx_class = "gem5::prefetch::IrregularStreamBuffer"
    cxx_header = "mem/cache/prefetch/irregular_stream_buffer.hh"

    num_counter_bits = Param.Unsigned(
        2, "Number of bits of the confidence counter"
    )
    chunk_size = Param.Unsigned(
        256, "Maximum number of addresses in a temporal stream"
    )
    degree = Param.Unsigned(4, "Number of prefetches to generate")
    training_unit_assoc = Param.Unsigned(
        128, "Associativity of the training unit"
    )
    training_unit_entries = Param.MemorySize(
        "128", "Number of entries of the training unit"
    )
    training_unit_indexing_policy = Param.TaggedIndexingPolicy(
        TaggedSetAssociative(
            entry_size=1,
            assoc=Parent.training_unit_assoc,
            size=Parent.training_unit_entries,
        ),
        "Indexing policy of the training unit",
    )
    training_unit_replacement_policy = Param.BaseReplacementPolicy(
        LRURP(), "Replacement policy of the training unit"
    )

    prefetch_candidates_per_entry = Param.Unsigned(
        16, "Number of prefetch candidates stored in a SP-AMC entry"
    )
    address_map_cache_assoc = Param.Unsigned(
        128, "Associativity of the PS/SP AMCs"
    )
    address_map_cache_entries = Param.MemorySize(
        "128", "Number of entries of the PS/SP AMCs"
    )
    ps_address_map_cache_indexing_policy = Param.TaggedIndexingPolicy(
        TaggedSetAssociative(
            entry_size=1,
            assoc=Parent.address_map_cache_assoc,
            size=Parent.address_map_cache_entries,
        ),
        "Indexing policy of the Physical-to-Structural Address Map Cache",
    )
    ps_address_map_cache_replacement_policy = Param.BaseReplacementPolicy(
        LRURP(),
        "Replacement policy of the Physical-to-Structural Address Map Cache",
    )
    sp_address_map_cache_indexing_policy = Param.TaggedIndexingPolicy(
        TaggedSetAssociative(
            entry_size=1,
            assoc=Parent.address_map_cache_assoc,
            size=Parent.address_map_cache_entries,
        ),
        "Indexing policy of the Structural-to-Physical Address Mao Cache",
    )
    sp_address_map_cache_replacement_policy = Param.BaseReplacementPolicy(
        LRURP(),
        "Replacement policy of the Structural-to-Physical Address Map Cache",
    )


class SlimAccessMapPatternMatching(AccessMapPatternMatching):
    start_degree = 2
    limit_stride = 4


class SlimDeltaCorrelatingPredictionTables(DeltaCorrelatingPredictionTables):
    table_entries = "256"
    table_assoc = 256
    deltas_per_entry = 9


class SlimAMPMPrefetcher(QueuedPrefetcher):
    type = "SlimAMPMPrefetcher"
    cxx_class = "gem5::prefetch::SlimAMPM"
    cxx_header = "mem/cache/prefetch/slim_ampm.hh"

    ampm = Param.AccessMapPatternMatching(
        SlimAccessMapPatternMatching(), "Access Map Pattern Matching object"
    )
    dcpt = Param.DeltaCorrelatingPredictionTables(
        SlimDeltaCorrelatingPredictionTables(),
        "Delta Correlating Prediction Tables object",
    )


class BOPPrefetcher(QueuedPrefetcher):
    type = "BOPPrefetcher"
    cxx_class = "gem5::prefetch::BOP"
    cxx_header = "mem/cache/prefetch/bop.hh"
    score_max = Param.Unsigned(31, "Max. score to update the best offset")
    round_max = Param.Unsigned(100, "Max. round to update the best offset")
    bad_score = Param.Unsigned(10, "Score at which the HWP is disabled")
    rr_size = Param.Unsigned(64, "Number of entries of each RR bank")
    tag_bits = Param.Unsigned(12, "Bits used to store the tag")
    offset_list_size = Param.Unsigned(
        46, "Number of entries in the offsets list"
    )
    negative_offsets_enable = Param.Bool(
        True,
        "Initialize the offsets list also with negative values \
                (i.e. the table will have half of the entries with positive \
                offsets and the other half with negative ones)",
    )
    delay_queue_enable = Param.Bool(True, "Enable the delay queue")
    delay_queue_size = Param.Unsigned(
        15, "Number of entries in the delay queue"
    )
    delay_queue_cycles = Param.Cycles(
        60,
        "Cycles to delay a write in the left RR table from the delay \
                queue",
    )

    # BOP is a degree one prefetcher
    degree = Param.Int(1, "Number of prefetches to generate")

    queue_squash = True
    queue_filter = True
    cache_snoop = True
    prefetch_on_pf_hit = True
    on_miss = True
    on_inst = False


class SmsPrefetcher(QueuedPrefetcher):
    # Paper: https://web.eecs.umich.edu/~twenisch/papers/isca06.pdf
    type = "SmsPrefetcher"
    cxx_class = "gem5::prefetch::Sms"
    cxx_header = "mem/cache/prefetch/sms.hh"
    ft_size = Param.Unsigned(64, "Size of Filter and Active generation table")
    pht_size = Param.Unsigned(16384, "Size of pattern history table")
    region_size = Param.Unsigned(4096, "Spatial region size")

    queue_squash = True
    queue_filter = True
    cache_snoop = True
    prefetch_on_access = True
    on_inst = False


class SBOOEPrefetcher(QueuedPrefetcher):
    type = "SBOOEPrefetcher"
    cxx_class = "gem5::prefetch::SBOOE"
    cxx_header = "mem/cache/prefetch/sbooe.hh"
    latency_buffer_size = Param.Int(32, "Entries in the latency buffer")
    sequential_prefetchers = Param.Int(9, "Number of sequential prefetchers")
    sandbox_entries = Param.Int(1024, "Size of the address buffer")
    score_threshold_pct = Param.Percent(
        25,
        "Min. threshold to issue a \
        prefetch. The value is the percentage of sandbox entries to use",
    )


class STeMSPrefetcher(QueuedPrefetcher):
    type = "STeMSPrefetcher"
    cxx_class = "gem5::prefetch::STeMS"
    cxx_header = "mem/cache/prefetch/spatio_temporal_memory_streaming.hh"

    spatial_region_size = Param.MemorySize(
        "2KiB", "Memory covered by a hot zone"
    )
    active_generation_table_entries = Param.MemorySize(
        "64", "Number of entries in the active generation table"
    )
    active_generation_table_assoc = Param.Unsigned(
        64, "Associativity of the active generation table"
    )
    active_generation_table_indexing_policy = Param.TaggedIndexingPolicy(
        TaggedSetAssociative(
            entry_size=1,
            assoc=Parent.active_generation_table_assoc,
            size=Parent.active_generation_table_entries,
        ),
        "Indexing policy of the active generation table",
    )
    active_generation_table_replacement_policy = Param.BaseReplacementPolicy(
        LRURP(), "Replacement policy of the active generation table"
    )

    pattern_sequence_table_entries = Param.MemorySize(
        "16384", "Number of entries in the pattern sequence table"
    )
    pattern_sequence_table_assoc = Param.Unsigned(
        16384, "Associativity of the pattern sequence table"
    )
    pattern_sequence_table_indexing_policy = Param.TaggedIndexingPolicy(
        TaggedSetAssociative(
            entry_size=1,
            assoc=Parent.pattern_sequence_table_assoc,
            size=Parent.pattern_sequence_table_entries,
        ),
        "Indexing policy of the pattern sequence table",
    )
    pattern_sequence_table_replacement_policy = Param.BaseReplacementPolicy(
        LRURP(), "Replacement policy of the pattern sequence table"
    )

    region_miss_order_buffer_entries = Param.Unsigned(
        131072, "Number of entries of the Region Miss Order Buffer"
    )
    add_duplicate_entries_to_rmob = Param.Bool(
        True, "Add duplicate entries to RMOB"
    )
    reconstruction_entries = Param.Unsigned(
        256, "Number of reconstruction entries"
    )


class HWPProbeEventRetiredInsts(HWPProbeEvent):
    def register(self):
        if self.obj:
            for name in self.names:
                self.prefetcher.getCCObject().addEventProbeRetiredInsts(
                    self.obj.getCCObject(), name
                )


class PIFPrefetcher(QueuedPrefetcher):
    type = "PIFPrefetcher"
    cxx_class = "gem5::prefetch::PIF"
    cxx_header = "mem/cache/prefetch/pif.hh"
    cxx_exports = [PyBindMethod("addEventProbeRetiredInsts")]

    prec_spatial_region_bits = Param.Unsigned(
        2, "Number of preceding addresses in the spatial region"
    )
    succ_spatial_region_bits = Param.Unsigned(
        8, "Number of subsequent addresses in the spatial region"
    )
    compactor_entries = Param.Unsigned(2, "Entries in the temp. compactor")
    stream_address_buffer_entries = Param.Unsigned(7, "Entries in the SAB")
    history_buffer_size = Param.Unsigned(16, "Entries in the history buffer")

    index_entries = Param.MemorySize("64", "Number of entries in the index")
    index_assoc = Param.Unsigned(64, "Associativity of the index")
    index_indexing_policy = Param.TaggedIndexingPolicy(
        TaggedSetAssociative(
            entry_size=1, assoc=Parent.index_assoc, size=Parent.index_entries
        ),
        "Indexing policy of the index",
    )
    index_replacement_policy = Param.BaseReplacementPolicy(
        LRURP(), "Replacement policy of the index"
    )

    def listenFromProbeRetiredInstructions(self, simObj):
        if not isinstance(simObj, SimObject):
            raise TypeError("argument must be of SimObject type")
        self.addEvent(
            HWPProbeEventRetiredInsts(self, simObj, "RetiredInstsPC")
        )


class FetchDirectedPrefetcher(BasePrefetcher):
    type = "FetchDirectedPrefetcher"
    cxx_class = "gem5::prefetch::FetchDirectedPrefetcher"
    cxx_header = "mem/cache/prefetch/fdp.hh"
    cxx_exports = [PyBindMethod("setCache")]

    def __init__(self, **kwargs):
        super().__init__(**kwargs)
        self._cache = None

    def regProbeListeners(self):
        if self._cache:
            self.getCCObject().setCache(self._cache.getCCObject())
        super().regProbeListeners()

    def registerCache(self, simObj):
        if not isinstance(simObj, SimObject):
            raise TypeError("argument must be a SimObject type")
        self._cache = simObj

    cpu = Param.BaseCPU(Parent.any, "The CPU to train the predictor")

    latency = Param.Cycles(1, "Latency for generated prefetches")
    pfq_size = Param.Unsigned(64, "Maximum number of queued prefetches")
    tq_size = Param.Unsigned(64, "Maximum number of outstanding translations")

    mark_req_as_prefetch = Param.Bool(
        True,
        "Mark memory requests as prefetches. Allows different handlings of "
        "request. E.g. the Arm TLB drops prefetch requests on a miss.",
    )
    squash_prefetches = Param.Bool(
        True,
        "Squash the prefetch associated with a fetch target in case it gets "
        "removded fron the the FTQ (Fetch consumes it or a pipeline flush).",
    )
    cache_snoop = Param.Bool(
        True,
        "Snoop the icache (if present) and do not enqueue prefetches for "
        "blocks already in the cache.",
    )


add_citation(
    FetchDirectedPrefetcher,
    """@inproceedings{10.1145/3613424.3614258,
  author    = {Schall, David and
               Sandberg, Andreas and
               Grot, Boris},
  title     = {Warming Up a Cold Front-End with Ignite},
  year      = {2023},
  publisher = {Association for Computing Machinery},
  address   = {Toronto, ON, Canada},
  doi       = {10.1145/3613424.3614258},
  booktitle = {Proceedings of the 56th Annual IEEE/ACM International Symposium on Microarchitecture (MICRO '23)},
  series    = {MICRO'23}
}
""",
)
