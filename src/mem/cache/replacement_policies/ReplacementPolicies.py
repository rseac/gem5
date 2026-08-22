# Copyright (c) 2018-2020 Inria
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

from m5.params import *
from m5.proxy import *
from m5.SimObject import SimObject


class BaseReplacementPolicy(SimObject):
    type = "BaseReplacementPolicy"
    abstract = True
    cxx_class = "gem5::replacement_policy::Base"
    cxx_header = "mem/cache/replacement_policies/base.hh"


class DuelingRP(BaseReplacementPolicy):
    type = "DuelingRP"
    cxx_class = "gem5::replacement_policy::Dueling"
    cxx_header = "mem/cache/replacement_policies/dueling_rp.hh"

    constituency_size = Param.Unsigned(
        "The size of a region containing one sample"
    )
    team_size = Param.Unsigned(
        "Number of entries in a sampling set that belong to a team"
    )
    replacement_policy_a = Param.BaseReplacementPolicy(
        "Sub-replacement policy A"
    )
    replacement_policy_b = Param.BaseReplacementPolicy(
        "Sub-replacement policy B"
    )


class FIFORP(BaseReplacementPolicy):
    type = "FIFORP"
    cxx_class = "gem5::replacement_policy::FIFO"
    cxx_header = "mem/cache/replacement_policies/fifo_rp.hh"


class SecondChanceRP(FIFORP):
    type = "SecondChanceRP"
    cxx_class = "gem5::replacement_policy::SecondChance"
    cxx_header = "mem/cache/replacement_policies/second_chance_rp.hh"


class LFURP(BaseReplacementPolicy):
    type = "LFURP"
    cxx_class = "gem5::replacement_policy::LFU"
    cxx_header = "mem/cache/replacement_policies/lfu_rp.hh"


class LRURP(BaseReplacementPolicy):
    type = "LRURP"
    cxx_class = "gem5::replacement_policy::LRU"
    cxx_header = "mem/cache/replacement_policies/lru_rp.hh"


class BIPRP(LRURP):
    type = "BIPRP"
    cxx_class = "gem5::replacement_policy::BIP"
    cxx_header = "mem/cache/replacement_policies/bip_rp.hh"
    btp = Param.Percent(3, "Percentage of blocks to be inserted as MRU")


class LIPRP(BIPRP):
    btp = 0


class MRURP(BaseReplacementPolicy):
    type = "MRURP"
    cxx_class = "gem5::replacement_policy::MRU"
    cxx_header = "mem/cache/replacement_policies/mru_rp.hh"


class RandomRP(BaseReplacementPolicy):
    type = "RandomRP"
    cxx_class = "gem5::replacement_policy::Random"
    cxx_header = "mem/cache/replacement_policies/random_rp.hh"


class BRRIPRP(BaseReplacementPolicy):
    type = "BRRIPRP"
    cxx_class = "gem5::replacement_policy::BRRIP"
    cxx_header = "mem/cache/replacement_policies/brrip_rp.hh"
    num_bits = Param.Int(2, "Number of bits per RRPV")
    hit_priority = Param.Bool(
        False, "Prioritize evicting blocks that havent had a hit recently"
    )
    btp = Param.Percent(
        3, "Percentage of blocks to be inserted with long RRPV"
    )


class RRIPRP(BRRIPRP):
    btp = 100


class DRRIPRP(DuelingRP):
    # The constituency_size and the team_size must be manually provided, where:
    #     constituency_size = num_cache_entries /
    #         (num_dueling_sets * num_entries_per_set)
    # The paper assumes that:
    #     num_dueling_sets = 32
    #     team_size = num_entries_per_set
    replacement_policy_a = BRRIPRP()
    replacement_policy_b = RRIPRP()


class NRURP(BRRIPRP):
    btp = 100
    num_bits = 1


class SHiPRP(BRRIPRP):
    type = "SHiPRP"
    abstract = True
    cxx_class = "gem5::replacement_policy::SHiP"
    cxx_header = "mem/cache/replacement_policies/ship_rp.hh"

    shct_size = Param.Unsigned(16384, "Number of SHCT entries")
    # By default any value greater than 0 is enough to change insertion policy
    insertion_threshold = Param.Percent(
        1, "Percentage at which an entry changes insertion policy"
    )
    # Always make hits mark entries as last to be evicted
    hit_priority = True
    # Let the predictor decide when to change insertion policy
    btp = 0


class SHiPMemRP(SHiPRP):
    type = "SHiPMemRP"
    cxx_class = "gem5::replacement_policy::SHiPMem"
    cxx_header = "mem/cache/replacement_policies/ship_rp.hh"


class SHiPPCRP(SHiPRP):
    type = "SHiPPCRP"
    cxx_class = "gem5::replacement_policy::SHiPPC"
    cxx_header = "mem/cache/replacement_policies/ship_rp.hh"


class TreePLRURP(BaseReplacementPolicy):
    type = "TreePLRURP"
    cxx_class = "gem5::replacement_policy::TreePLRU"
    cxx_header = "mem/cache/replacement_policies/tree_plru_rp.hh"
    num_leaves = Param.Int(Parent.assoc, "Number of leaves in each tree")


class StreamDemoteLRURP(LRURP):
    """LRU that demotes lines of registered stream pages to the LRU
    position — pages the Viper/GDP prefetcher publishes through the
    shared VectorChainTable as it issues index-array stream prefetches
    (unit-stride vector arrays: single-use, dead after their access).
    demote_on_insert=True is the L2 mode (a stream line's L2 copy is
    dead on arrival: its demand use is served by the L1 copy);
    demote_on_insert=False is the L1 mode (insert normally, demote at
    the first touch — a unit-stride vector access consumes the whole
    line, making that touch an architecturally known last use).
    Non-stream lines behave as plain LRU."""

    type = "StreamDemoteLRURP"
    cxx_class = "gem5::replacement_policy::StreamDemoteLRU"
    cxx_header = "mem/cache/replacement_policies/stream_demote_lru_rp.hh"

    link_table = Param.VectorChainTable(
        NULL,
        "The per-core chain table publishing stream physical pages "
        "(same instance as the prefetcher's link_table and the CPU's "
        "vector_chain_table).",
    )
    demote_on_insert = Param.Bool(
        False,
        "Demote stream lines at insertion (L2 semantics) instead of "
        "at their first touch (L1 semantics).",
    )
    second_touch_promote = Param.Bool(
        False,
        "Promote a demoted stream line on its SECOND touch while "
        "resident (per-residency consumed bit): a line the single-use "
        "oracle declared dead that gets used again is observed "
        "cross-sweep reuse (iterative kernels), and holding it demoted "
        "churns exactly the lines that keep coming back. True "
        "single-use streams never see a second touch, so demotion "
        "wins are preserved.",
    )
    page_promote = Param.Bool(
        False,
        "Second touch also unlearns the whole PAGE "
        "(VectorChainTable.promoteStreamPage): removed from the stream "
        "registry and blocked from re-registration, so NEW fills of a "
        "proven-reused page insert as plain LRU. Per-line promotion "
        "alone protects incumbents but gives evicted lines no re-entry "
        "path (they re-insert demoted and are re-victimized before "
        "their second touch). Requires second_touch_promote.",
    )


class WeightedLRURP(LRURP):
    type = "WeightedLRURP"
    cxx_class = "gem5::replacement_policy::WeightedLRU"
    cxx_header = "mem/cache/replacement_policies/weighted_lru_rp.hh"
