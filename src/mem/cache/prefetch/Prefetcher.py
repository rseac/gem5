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

    degree = Param.Int(16, "Number of prefetches to generate")
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
    addr_array_len = Param.Unsigned(64, "Number of misses tracked")
    prefetch_threshold = Param.Unsigned(
        2, "Counter threshold to start the indirect prefetching"
    )
    stream_counter_threshold = Param.Unsigned(
        4, "Counter threshold to enable the stream prefetcher"
    )
    streaming_distance = Param.Unsigned(
        16, "Number of prefetches to generate when using the stream prefetcher"
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


class ViperFinalPrefetcher(QueuedPrefetcher):
    """VIPER-FINAL: the shipping subset of the research VIPER prefetcher
    (``ViperPrefetcher``, removed 2026-09-04).

    Bit-for-bit the same A[B[i]] engine as that design -- architectural
    discovery through the shared VectorChainTable, the collapsed
    ``target = base + (extend(idx) >> r << s)`` form, range-based fill
    capture into per-producer index line queues (ILQ), and whole-line conversion
    through the shift-and-add lane array -- with five exploratory
    subsystems REMOVED rather than defaulted off:

      * the scalar level chain (``scalar_chain_enable`` and its
        TycheChainTable sideband, head trigger, Form B terminal hop,
        and the ``demand_span_chained`` gate it armed);
      * residency-interval registers (``residency_intervals``);
      * the set-pressure gate (``set_pressure_threshold``);
      * stream-page registry publication (the ``registerStreamPage``
        call that feeds StreamDemoteLRURP);
      * DRAM row-aware issue reordering (``row_schedule_bits``).

    The emission policy is settled too: the run-ahead stream walk is
    the only one. Viper's ``demand_span_only`` ablation -- replace the
    walk with completion of the current chunk's own span -- is gone as
    well; it won on short-row traversals and lost badly on long
    contiguous rows and on pure streams, so it is a workload-specific
    alternative rather than a shipping policy. Run that comparison on
    ``viper``.

    One deliberate difference beyond the removals: ``prefetch_distance``
    is the only lookahead knob (viper's access-size-relative
    ``streaming_distance`` walk is gone, so the frontier means the same
    number of bytes at every VLEN and vl). Since 2026-09-12 it is
    denominated in CACHE LINES; the whole-VLEN chunk unit and its
    ``vlen`` parameter are gone, so no VLEN reaches the prefetcher.

    Forms come only from the chain table's backward walk: the
    VectorChainTable must have ``folded_forms`` on (the config script
    sets it for this prefetcher), and each pushed form lives in ONE
    place, an IPT way here. A consumer way is live iff that record
    holds a valid form and a producer holds a slot iff it holds an IPT
    set, so there is no op-list adoption path, no per-way configured
    bit and no slot counter (2026-09-02 trim; see viper_final.hh).

    Only unit-stride index loads are tracked: a strided (vlse)
    producer issues one access per element, which needs a span model
    the walk does not have.

    Everything else is preserved verbatim, including cross-line,
    pre-lane and
    eviction-clocked dedup, the random tail-drop throttle, the
    announced-limit gate, and multi-way consumers. Removing the
    scalar chain also removes this prefetcher's only dependence on
    TycheChainTable, so it wires with the VectorChainTable alone.

    Vector-side design (requires --prefetcher-side vector). Wire ONE
    VectorChainTable per core to both this prefetcher's link_table
    and the CPU's vector_chain_table. See
    mem/cache/prefetch/viper_final.hh."""

    type = "ViperFinalPrefetcher"
    cxx_class = "gem5::prefetch::ViperFinal"
    cxx_header = "mem/cache/prefetch/viper_final.hh"

    # Index loads are data reads; ignore instruction accesses.
    on_inst = False
    # The stream walk needs every chunk event, hit or miss.
    prefetch_on_access = True
    # Index values are virtual; train on VAs and translate targets
    # through the registered MMU.
    use_virtual_addresses = True
    queue_size = 64

    link_table = Param.VectorChainTable(
        NULL,
        "CPU-side chain/link table (must be the same instance as the "
        "CPU's vector_chain_table param). Shared with GDP: the discovery half "
        "of the two designs is identical.",
    )
    prefetch_distance = Param.Unsigned(
        8,
        "Stream frontier distance in CACHE LINES (N * cache line size "
        "bytes ahead of the demand cursor) and the design's ONLY "
        "lookahead knob -- VIPER-FINAL has no access-size-relative "
        "streaming_distance walk and no VLEN-relative chunk unit "
        "(2026-09-12; it was whole-VLEN chunks with a vlen param). Each "
        "chunk event walks the window [addr + N*line, addr + size + "
        "N*line), so partial-vl chunks (short spmv rows, loop tails) "
        "keep the full byte lookahead and the knob means the same "
        "number of bytes at every VLEN; steady-state degree is the "
        "demand advance. Must be >= 1. Default 8 lines = 512 B, the "
        "measured spmv sweet spot at 64-entry queue / 16 MSHRs. Full "
        "indirect timeliness needs distance x chunk-cadence to cover "
        "TWO memory round trips (the index line, then the target).",
    )
    stream_only = Param.Bool(
        False,
        "Ablation: stream the index arrays but never capture/convert "
        "(isolates the architectural-stream contribution)",
    )
    index_line_queue_entries = Param.Unsigned(
        8,
        "Captured raw index lines each producer's index line queue "
        "(ILQ) holds. This is GDP's slice_buffer_entries renamed: the "
        "staging is identical, only the drain's compute engine "
        "differs. A fill arriving with the ILQ full is dropped "
        "whole (bufferBusyDrops).",
    )
    producer_slots = Param.Unsigned(
        2,
        "Concurrently configured producers: the number of IPT sets. "
        "The IPT lives in this prefetcher, beside the lane array that "
        "reads it; the discovery half pushes a folded form across once "
        "for each chain, and a set (found by producer PC) holds one way "
        "per consumer of that producer. Holding a set IS holding a "
        "slot, so there is no separate slot counter: a push that finds "
        "every set taken is dropped (iptSetConflicts) and that producer "
        "streams without converting. Each slot costs one dedup record "
        "per consumer way plus an ILQ, so this is the main area knob "
        "after index_line_queue_entries.",
    )
    routing_entries = Param.Unsigned(
        64,
        "Index Routing Table (IRT) capacity: index lines registered "
        "for capture; a matching fill is routed to the owning "
        "producer's conversion array",
    )
    capture_on_read_hit = Param.Bool(
        False,
        "Also capture an index line off the cache read port when the "
        "demand read HITS, instead of only from a fill. An already "
        "resident index line never produces a fill, so the IRT entry "
        "registered for it can never latch - the dominant capture loss "
        "on multi-chain loops, where one producer's stream walk makes "
        "the sibling producers' index lines resident before their own "
        "demands reach them (s353 at pipelines=5: 18740 lines "
        "registered, only 4855 latched). A read hit carries the bytes, "
        "the VA and the PA together, so the line is latched straight "
        "into the index line queue with no IRT round trip (hitsCaptured). "
        "This buys no lookahead over a fill capture - the hit is at the "
        "owning producer's own cursor - the gain is that the sibling "
        "chains get index data at all.",
    )
    retired_tail_entries = Param.Unsigned(
        0,
        "Retired-IRT tail depth: index lines whose conversion already "
        "ran are remembered here (line PA + owning producer PC) after "
        "leaving the pending IRT, instead of being erased outright. "
        "The tail is what capture_read_hit_own_producer consults; 0 "
        "disables it (recaptures are then never detected). FIFO, so a "
        "line converted long enough ago ages out and a later read hit "
        "converts it again - the correct answer for a genuine revisit.",
    )
    capture_read_hit_own_producer = Param.Bool(
        False,
        "capture_on_read_hit disposition when the retired tail says "
        "THIS producer already converted the line: true = capture "
        "anyway (the behaviour before the tail existed), false = skip "
        "it (recapturesSuppressed). The double conversion it removes "
        "is self-inflicted: the prefetcher's own stream walk fetches "
        "the index line, the fill converts it, and the demand read "
        "that follows then hits and converts the identical elements a "
        "second time - so the better the streaming half works, the "
        "more of it there is. A retired entry from a DIFFERENT "
        "producer never suppresses, which is what keeps the "
        "multi-chain case (s353) that capture_on_read_hit exists for.",
    )
    capture_queue_gate = Param.Unsigned(
        0,
        "Capture admission backpressure: skip new captures (IRT "
        "registration on the prefetch-window path, and read-hit "
        "capture) while the prefetch queue "
        "backlog - queued targets plus targets awaiting translation - "
        "is at or above this many entries. 0 disables (default, "
        "bit-neutral). A capture admitted while the queue is "
        "saturated is work that issues late: emission stalls, the "
        "demand cursor passes the captured line, and its targets go "
        "out behind the demand (until 2026-09-05 a staleness abort "
        "discarded such batches - up to half of all captures at L1W1 "
        "in the TSVC LARGE campaign - measured a no-op and deleted). "
        "The stream walk itself is never gated, only the "
        "capture/convert half. Guidance: queue_size minus one line's "
        "worth of targets (64 - 16 = 48 for int32 indices) admits a "
        "capture only when its conversion could actually queue.",
    )
    drop_batch_confidence = Param.Unsigned(
        0,
        "Batch-granular residency feedback: when the cache drops one "
        "of this prefetcher's targets as already resident "
        "(pfHitInCache), count it against the BATCH that generated it "
        "- the up-to-blkSize/EEW targets converted from one captured "
        "index line. After this many resident-drops from the same "
        "batch, the batch's remaining targets are flushed from the "
        "prefetch queue and its still-unemitted targets are "
        "suppressed (targetsFlushedOnDrop). Rationale: measured "
        "residency is all-or-nothing per index line (one drop "
        "predicts the sibling targets are resident too), so one probe "
        "pays for the rest of the batch. 0 disables (default); 1 = "
        "abort on the first drop; higher values demand more evidence "
        "before sacrificing a batch (partial-residency workloads). "
        "Stream candidates are never touched - only converted "
        "indirect targets carry a batch tag. Targets already parked "
        "awaiting translation leak through (their queue cannot be "
        "edited mid-translation) and are dropped by the cache as "
        "before, bounded by the translation queue depth.",
    )
    drop_batch_fraction = Param.Float(
        1.0,
        "Fraction of a batch's remaining targets discarded once "
        "drop_batch_confidence is reached, in (0, 1]. 1.0 (default) "
        "forfeits the whole remainder - the original all-or-nothing "
        "abort. Smaller values thin the remainder evenly (a Bresenham "
        "accumulator per aborted batch drops exactly this fraction, "
        "spread across both the queue flush and the still-unemitted "
        "suppression), for workloads where residency is only partial "
        "per index line and a whole-batch abort sacrifices needed "
        "siblings (the poisson3Db failure mode: 28.6/32 unique lines "
        "per chunk, dbc1 cost 0.94 -> 0.71 coverage). Only read when "
        "drop_batch_confidence != 0.",
    )
    drain_floor = Param.Int(
        0,
        "Minimum targets emitted per access event when the prefetch "
        "queue is full (bounded displacement). Models the continuous "
        "queue drain of real hardware, which gem5's pull-only queue "
        "lacks: at 0 the emission wedges precisely when miss pressure "
        "is highest. Set 0 for a pure stall-on-full.",
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
    conversion_lanes = Param.Unsigned(
        0,
        "Shift-and-add lane circuits in the conversion array: at "
        "most this many index ELEMENTS convert to target addresses "
        "per drain event; a captured line wider than the budget "
        "stays at the ILQ front and resumes next event "
        "(conversionWidthLimited counts the cut-shorts). Within-line "
        "dedup and the dedup window keep whole-line semantics across "
        "the split. 0 = unbounded (whole line per event, the classic "
        "Viper array; 8 = one full e64 line per event). GDP's "
        "pipelines_per_gather analog for the collapsed-chain array; "
        "same semantics as VHybrid's conversion_lanes.",
    )
    drop_on_full = Param.Bool(
        False,
        "Queue-full disposition for converted indirect targets. True: "
        "the overflow target is DROPPED and the queue keeps its "
        "staged entries — protects stream lines from target-burst "
        "displacement in the shared queue (targetsDroppedFull counts "
        "them; dropped targets are not recorded in the dedup window "
        "since no prefetch went out, so later duplicates may still "
        "re-emit). False (default): the drain_floor license applies — "
        "up to drain_floor targets push into the full queue per "
        "event, evicting its oldest entries (pfRemovedFull). "
        "drain_floor=0 remains the third disposition: stall, holding "
        "targets latched until room appears.",
    )
    strided_producers = Param.Bool(
        True,
        "Track strided (vlse) index producers (2026-09-12): a chunk is "
        "the span of vl elements at the captured byte stride, the walk "
        "keeps the same bytes-of-index lookahead as unit-stride, and "
        "captured lines carry a slot mask so only the index slots "
        "convert. Only producers with a linked gather stream. False "
        "rejects strided producers as before (bit-neutral for "
        "unit-stride code either way).",
    )
    segment_producers = Param.Bool(
        True,
        "Track unit-stride segment (vlseg<nf>e<eew>) index producers "
        "(2026-09-16). Memory-side a vlseg is a plain unit-stride "
        "stream (contiguous requests over the interleaved region), so "
        "chunk detection, the walk and capture are unchanged; a "
        "captured line carries the field-0 base slot mask (one bit "
        "every nf slots, phased by the macro base the chain table "
        "captures at commit) and each consumer way converts only the "
        "slots of the field its gather consumed (FoldedForm::field, "
        "3 bits, threaded PT -> DCT -> IPT way). A way's mask is the "
        "base mask rotated by its field id within the nf-slot period, "
        "derived at the head of the ILQ, never stored. Off-field slots "
        "fire no lane and cost no budget (fieldSlotsSkipped). False "
        "rejects segment producers (segmentUnsupported); bit-neutral "
        "for code without vlseg either way.",
    )
    pre_lane_dedup = Param.Bool(
        False,
        "Placement of the per-line seen-set compare relative to the "
        "shift/add lane. The compare itself is the same either way: "
        "the seen-set holds folded index keys "
        "((extend(idx)<<shift) + (base & 63)) >> 6, which name target "
        "lines exactly (line = key + base>>6), so the emitted set does "
        "not depend on this knob. True = the CAM sits BEFORE the lanes "
        "and a hit clock-gates the lane, spending no conversion_lanes "
        "slot (slotsCompacted counts elements every way gated on); "
        "False = the lane fires first and the hit is dropped after it, "
        "so it spends a slot. Only differs at conversion_lanes > 0.",
    )
    seen_set_entries = Param.Unsigned(
        0,
        "Scope of the per-way seen-set (the intra-line coalescing CAM "
        "of folded index keys). 0 = per source line: the CAM clears "
        "when each captured index line's first chunk converts, so it "
        "holds at most blkSize/EEW keys and needs no replacement. "
        "N > 0 = a persistent FIFO CAM of N keys per consumer way "
        "that does NOT clear between index lines: an element whose "
        "target line was converted from an earlier index line is "
        "dropped (targetsDeduplicated) for as long as its key stays "
        "in the CAM, and the oldest key is replaced when the CAM is "
        "full (seenSetReplaced). Unlike dedup_buffer_size the check "
        "is emission-blind: a key enters at conversion, so a target "
        "the emission stage dropped (drop_on_full, "
        "drop_batch_confidence, evict_dedup_entries) cannot re-emit "
        "while its key is held. The CAM still clears when its keys "
        "stop naming target lines: a form re-push with different "
        "operands, a producer flush, or a backward demand jump "
        "(together with the dedup window).",
    )
    seen_set_dedup = Param.Bool(
        False,
        "Ablation: False disables the per-way seen-set entirely (no "
        "compare, no insert), so every element fires its lane and "
        "every converted target reaches the latch. Intra-line "
        "duplicates are then caught only by the emission-side "
        "structures: dedup_buffer_size cannot see the line in "
        "progress, so within one source line only evict_dedup_entries "
        "can suppress them (the first copy is emitted and inserted, "
        "the rest hit the table). targetsDeduplicated reads 0.",
    )
    consumers_per_producer = Param.Unsigned(
        4,
        "Consumer ways per producer index load: shift/add lane "
        "groups fed by ONE broadcast ILQ, one group (own "
        "base/shift/ext form + dedup state) per linked gather, so "
        "multi-way patterns like A[B[i]] and C[B[i]] prefetch every "
        "target array. Since 2026-09-16 a way is a way/field: it also "
        "names the producer field it consumes (segment_producers), so "
        "4 ways cover two gathers on two vlseg2 fields, four gathers "
        "on one index load, or any mix. Default raised 2 -> 4 the "
        "same day (bit-neutral for producers with <= 2 gathers). "
        "Emission is element-major interleaved "
        "(A[B[0]], C[B[0]], A[B[1]], ...). conversion_lanes counts "
        "ELEMENT slots shared across ways, charged on any-miss: an "
        "element every way's pre-lane CAM gates on is compacted out "
        "free (slotsCompacted). The index side (capture, IRT, walk, "
        "staging) is never duplicated. Must match the chain table's "
        "consumers_per_producer — the config script copies this "
        "value onto the table it wires. 1 (default) = the classic "
        "single-way behavior, bit-identical stats.",
    )
    evict_dedup_entries = Param.Unsigned(
        0,
        "Eviction-clocked dedup table capacity in target lines (0 "
        "disables, default, bit-neutral). A global LRU table of "
        "emitted indirect target lines whose entries live until the "
        "CACHE reports the line gone, instead of falling off a "
        "fixed-depth window: a tracked line's re-emission is "
        "suppressed (targetsEvictDeduplicated) for exactly as long "
        "as it would be wasted, at any reuse distance - the "
        "long-range recurrence dedup_buffer_size cannot see. "
        "Entries insert when a target is actually pushed, refresh "
        "on suppression hits and observed demand HITs to the line "
        "(LRU-by-access: recurring resident lines survive "
        "capacity), and clear on the cache's eviction notification "
        "(the proactive re-arm) or an observed demand MISS to the "
        "line (the corrective re-arm for evictions the PA side "
        "index never saw). Replacement and every other "
        "approximation degrade toward emitting - a forgotten line "
        "re-emits once and squashes as pfHitInCache - so the only "
        "cost of undersizing is redundant emissions, never lost "
        "coverage. Sizing: the table can never usefully exceed the "
        "attach cache's line count (512 for a 32 KiB L1); smaller "
        "values keep only the hottest recurring lines. Orthogonal "
        "to dedup_buffer_size (per-way, fixed window) and to the "
        "residency interval registers (span-granular, "
        "verdict-trained); all three compose.",
    )
    random_drop_interval = Param.Unsigned(
        0,
        "Random tail-drop throttle epoch length, in observed demand "
        "accesses (0 disables, default, bit-neutral). Every this many "
        "notify() events the throttle redraws its drop scale uniformly "
        "from random_drop_rates; the first epoch is always drop-free. "
        "While the drawn scale s is nonzero, each captured index line "
        "forfeits a uniform-random 1..floor(elems*s) elements off its "
        "TAIL before conversion (one draw per line, at its first "
        "chunk) - a randomized per-gather prefetch-degree clamp. "
        "Dropped elements never reach a shift/add lane and are never "
        "recorded emitted, so a later recapture of the line may still "
        "convert them (linesRandomTruncated, elementsRandomDropped).",
    )
    random_drop_rates = VectorParam.Float(
        [0.0, 1.0],
        "Candidate drop scales the epoch clock draws from uniformly "
        "(each in [0, 1]). A scale of 0 makes the epoch drop-free; "
        "scale s caps the per-line tail drop at floor(elems*s) "
        "elements. The default {0, 1} set is on/off duty cycling; a "
        "single-element set holds one scale for every epoch. Only "
        "read when random_drop_interval is nonzero. Pass via "
        "--pf-param as a comma list, e.g. random_drop_rates=0,0.5,1.",
    )
    random_drop_geometric = Param.Bool(
        False,
        "Randomize the epoch length itself: instead of redrawing the "
        "drop scale every random_drop_interval accesses exactly, each "
        "observed access redraws with probability 1/interval, giving "
        "memoryless (geometrically distributed) epoch lengths with "
        "the same mean. This breaks phase-locking between the epoch "
        "clock and the workload's own periodicity (strip-mined loops "
        "process a fixed number of accesses per strip, so a fixed-N "
        "clock can land the same scale on the same program phase "
        "every time). False (default) = the fixed counter, "
        "bit-neutral. Only meaningful when random_drop_interval is "
        "nonzero.",
    )
    random_drop_min_interval = Param.Unsigned(
        0,
        "Minimum epoch length in observed demand accesses (geometric "
        "mode only; 0 = no floor, the pure geometric). After a "
        "redraw, the epoch clock waits this many accesses before "
        "Bernoulli rolls resume, making epoch lengths "
        "min + Geometric(1/interval) — mean min + interval. Skews the "
        "distribution away from the geometric's short-epoch mass, "
        "where a drop epoch ends before it can affect more than a "
        "couple of captured lines (pure coverage loss, no measurable "
        "throttling), while keeping the memoryless tail that "
        "prevents phase-locking. Ignored in fixed mode.",
    )
    random_drop_seed = Param.Unsigned(
        1,
        "Seed for the throttle's private RNG (epoch scale draws and "
        "per-line tail-drop counts). A private engine keeps the draw "
        "sequence independent of everything else in the simulation, "
        "so two configs differ only in this mechanism; vary the seed "
        "to sample a different random schedule.",
    )
    stream_table = Param.RevelaStreamTable(
        NULL,
        "Optional CPU-side Stream Tracking Table (ReVeLA's "
        "announcement sideband; must be the same instance as the "
        "CPU's revela_table param). Only read when limit_gate is "
        "true; the config script wires it on demand.",
    )
    limit_gate = Param.Bool(
        False,
        "Clamp the stream WALK at the vsetvl-AVL announced extent end "
        "(default off, bit-neutral; needs the RevelaStreamTable "
        "sideband, which the config script wires when this is set). "
        "An access with no covering announcement has its whole walk "
        "suppressed. Note this bounds only where the walk may GO: "
        "unlike viper, VIPER-FINAL does not also clamp the SLICING of "
        "a captured line at the extent. A line is captured because it "
        "sat inside a producer's own window, or because the demand "
        "missed on it at a producer PC, so its bytes are that "
        "producer's index array by construction and are converted "
        "whole.",
    )


class ViperRtlPrefetcher(BasePrefetcher):
    """VIPER-FINAL as a Verilated RTL model (viper_rtl-hook).

    Same algorithm, parameters and statistics as ViperFinalPrefetcher,
    executed by the SystemVerilog implementation in the viper_rtl
    repository (rtl/), which also implements the QueuedPrefetcher
    queue. The parameters below are the ViperFinal knobs plus the
    Queued knobs the RTL honours; see ViperFinalPrefetcher for the
    long descriptions. Table capacities are fixed when the RTL is
    verilated (make verilate STT_ENTRIES=.. in the repo); a run whose
    parameter exceeds a capacity fails at startup with a message.

    Wire exactly like viper_final: --prefetcher viper_rtl on the vector
    side, the CPU's VectorChainTable as link_table (folded_forms on),
    vlen from --vlen."""

    type = "ViperRtlPrefetcher"
    cxx_class = "gem5::prefetch::ViperRtl"
    cxx_header = "mem/cache/prefetch/viper_rtl.hh"

    on_inst = False
    prefetch_on_access = True
    use_virtual_addresses = True

    # QueuedPrefetcher knobs implemented by the RTL queue
    latency = Param.Int(1, "Latency for generated prefetches")
    queue_size = Param.Int(64, "Maximum number of queued prefetches")
    queue_squash = Param.Bool(True, "Squash queued prefetch on demand access")
    queue_filter = Param.Bool(True, "Don't queue redundant prefetches")
    cache_snoop = Param.Bool(False, "Snoop cache to eliminate redundant request")
    tag_prefetch = Param.Bool(True, "Tag prefetch with PC of generating access")
    throttle_control_percentage = Param.Percent(0, "Not implemented in the RTL port; must be 0")

    link_table = Param.VectorChainTable(NULL, "CPU-side chain table (same instance as the CPU's vector_chain_table)")
    prefetch_distance = Param.Unsigned(4, "Stream frontier distance in whole-VLEN chunks (>= 1)")
    vlen = Param.Unsigned(0, "Hardware VLEN in bits (wired from --vlen)")
    stream_only = Param.Bool(False, "Ablation: stream the index arrays but never capture/convert")
    index_line_queue_entries = Param.Unsigned(2, "Captured raw index lines per producer slot (ILQ)")
    producer_slots = Param.Unsigned(2, "Concurrently configured producers (IPT sets)")
    routing_entries = Param.Unsigned(32, "Index Routing Table capacity")
    capture_on_read_hit = Param.Bool(False, "Also capture index lines off the read port on a demand read hit")
    retired_tail_entries = Param.Unsigned(32, "Retired-IRT tail depth (0 disables)")
    capture_read_hit_own_producer = Param.Bool(True, "Read-hit capture of a line this producer already converted")
    capture_queue_gate = Param.Unsigned(0, "Skip captures while the queue backlog is at or above this (0 disables)")
    drop_batch_confidence = Param.Unsigned(0, "Resident-drops per batch before its remaining targets are flushed (0 disables)")
    drop_batch_fraction = Param.Float(1.0, "Fraction of an aborted batch's remaining targets discarded, in (0, 1]")
    drain_floor = Param.Int(8, "Minimum targets emitted per access event when the queue is full")
    dedup_buffer_size = Param.Unsigned(0, "Cross-line dedup window in index lines (0 disables)")
    conversion_lanes = Param.Unsigned(0, "Index elements converted per drain event (0 = whole line)")
    drop_on_full = Param.Bool(False, "Drop overflow targets instead of displacing queued entries")
    pre_lane_dedup = Param.Bool(False, "Seen-set compare before the lane (a hit spends no lane slot)")
    consumers_per_producer = Param.Unsigned(1, "Consumer ways per producer (must match the chain table)")
    evict_dedup_entries = Param.Unsigned(0, "Eviction-clocked dedup table capacity (0 disables)")
    random_drop_interval = Param.Unsigned(0, "Random tail-drop epoch length in accesses (0 disables)")
    random_drop_rates = VectorParam.Float([0.0, 1.0], "Candidate drop scales (each in [0, 1])")
    random_drop_geometric = Param.Bool(False, "Memoryless epoch lengths")
    random_drop_min_interval = Param.Unsigned(0, "Minimum epoch length (geometric mode)")
    random_drop_seed = Param.Unsigned(1, "Seed of the RTL's xorshift64 RNG")
    stream_table = Param.RevelaStreamTable(NULL, "Announcement sideband, read when limit_gate is true")
    limit_gate = Param.Bool(False, "Clamp the stream walk at the announced extent end")
    trace_file = Param.String("", "FST waveform file (needs a TRACE=1 build of the model); empty = off")


class RevelaPrefetcher(QueuedPrefetcher):
    """ReVeLA: the Register Vector Length Agnostic prefetcher
    (Martinez Palau et al., ICS'24). Exploits the gap between the
    Requested Vector Length (the AVL a strip-mined VLA loop passes to
    vsetvl — the elements still to process) and the granted vl: a
    unit-stride vector access at access@ announces with certainty
    that [access@ + vl*eew, access@ + AVL*eew) will be accessed by
    the following iterations.

    The CPU side (RevelaStreamTable, cpu/revela_table.hh) snoops the
    AVL at vsetvl issue and tracks streams in a Stream Tracking Table
    updated by the LSQ at translation finish. This cache side runs
    the paper's trigger logic on a self-clocked every-cycle drain:
    whenever the prefetch queue has room, the streams at the minimum
    prefetched distance each advance one line, capped by an
    aggressivity limit that shrinks as more streams go live
    (64/32/16/8 lines at <2/<4/<8/>=8 valid entries).

    Near-perfect accuracy by construction (only announced lines are
    requested), limited coverage — the paper positions it as a
    COMPLEMENT to a conventional coverage prefetcher, not a
    replacement. Vector-side design (requires --prefetcher-side
    vector in the split hierarchy). Wire ONE RevelaStreamTable per
    core to both this prefetcher's stream_table and the CPU's
    revela_table. See mem/cache/prefetch/revela.hh."""

    type = "RevelaPrefetcher"
    cxx_class = "gem5::prefetch::ReVeLA"
    cxx_header = "mem/cache/prefetch/revela.hh"

    # Vector data streams only; instruction accesses are irrelevant.
    on_inst = False
    # Every demand access must notify (hit or miss): notifies latch
    # the drain's translation context and re-arm the drain event.
    prefetch_on_access = True
    # Streams are VA-contiguous but not PA-contiguous across page
    # frames (paper Section 3.5): track VAs and translate through the
    # registered MMU.
    use_virtual_addresses = True
    # The paper's 16-entry prefetch queue.
    queue_size = 16

    stream_table = Param.RevelaStreamTable(
        NULL,
        "CPU-side Stream Tracking Table (must be the same instance as "
        "the CPU's revela_table param; the config script wires both).",
    )
    max_prefetch_distance = Param.Unsigned(
        64,
        "Aggressivity ceiling in cache lines: the farthest a stream "
        "may run ahead of its last demand access when fewer than 2 "
        "streams are live; /2, /4, /8 as the valid STT entry count "
        "crosses 2, 4, 8 (the paper's Aggressivity Table, swept 8-128 "
        "in its sensitivity analysis).",
    )
    degree = Param.Unsigned(
        1,
        "Lines emitted per minimum-distance stream per trigger "
        "evaluation (the paper emits 1 per stream per cycle).",
    )
    initial_distance = Param.Unsigned(
        0,
        "Emission floor in cache lines ahead of a stream's demand "
        "cursor: at stream entry and after every demand catch-up the "
        "frontier jumps to current@ + this many lines instead of "
        "ramping from current@, skipping the doomed-late emissions a "
        "distance-zero ramp spends its bandwidth on (Viper's "
        "streaming_distance analogue). Skipped lines are never "
        "prefetched — demand pays their full miss. If it exceeds the "
        "aggressivity limit, the ceiling is raised to it (the stream "
        "degenerates to a fixed-offset follower). 0 = paper behavior.",
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
