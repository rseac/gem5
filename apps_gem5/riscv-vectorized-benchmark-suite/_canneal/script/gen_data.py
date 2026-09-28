#!/usr/bin/env python3
# Copyright 2026 ETH Zurich and University of Bologna.
#
# SPDX-License-Identifier: Apache-2.0
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#    http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

# Author: Navaneeth Kunhi Purayil
# Generates RISC-V assembly (.S file) with pre-computed index-based netlist data.
# All name-to-index resolution and fanin/fanout/fan_locs computation is done here
# so the C constructor can use simple indexed loops (no string lookups at runtime).

import sys
import os
import numpy as np

def emit(name, array, alignment='8'):
    """Emit binary data as assembly using .word directives."""
    print(".global %s" % name)
    print(".balign " + alignment)
    print("%s:" % name)
    bs = array.tobytes()
    for i in range(0, len(bs), 4):
        s = ""
        for n in range(4):
            s += "%02x" % bs[i+3-n]
        print("    .word 0x%s" % s)

def read_canneal_input(file_path):
    """Read CANNEAL input file with nets and connections."""
    nets = []
    with open(file_path, 'r') as f:
        lines = f.readlines()
        if not lines:
            return None, None, None, None

        header = lines[0].strip().split()
        num_elements = int(header[0])
        max_x = int(header[1])
        max_y = int(header[2])

        for i in range(1, num_elements):
            parts = lines[i].strip().split()
            if len(parts) < 2:
                continue
            net_name = parts[0]
            element_type = int(parts[1])
            connections = []
            for j in range(2, len(parts)):
                if parts[j] != "END":
                    connections.append(parts[j])
            nets.append({
                'name': net_name,
                'type': element_type,
                'connections': connections
            })

    return num_elements, max_x, max_y, nets

def emit_string_array(name, strings, alignment='8'):
    """Emit an array of pointers to strings."""
    print(".global %s" % name)
    print(".balign " + alignment)
    print("%s:" % name)
    for i, _ in enumerate(strings):
        print("    .quad .Lstr_%s_%d" % (name, i))
    print()
    for i, s in enumerate(strings):
        escaped = s.replace('\\', '\\\\').replace('"', '\\"').replace('\n', '\\n').replace('\t', '\\t')
        print(".Lstr_%s_%d:" % (name, i))
        print('    .asciz "%s"' % escaped)

def build_index_data(nets):
    """
    Pre-compute all index-based netlist data.
    Replicates the exact element ordering that the old C create_elem_if_necessary()
    would produce: named elements first (in input order), then any connection-only
    elements that were not in the named set.
    """
    name_to_index = {}
    next_idx = [0]

    def get_or_create(name):
        if name not in name_to_index:
            name_to_index[name] = next_idx[0]
            next_idx[0] += 1
        return name_to_index[name]

    # Phase 1: named elements in input order
    for net in nets:
        get_or_create(net['name'])
    # Phase 2: connection-only elements
    for net in nets:
        for conn in net['connections']:
            get_or_create(conn)

    total_used = next_idx[0]
    idx_to_name = {v: k for k, v in name_to_index.items()}
    all_names = [idx_to_name[i] for i in range(total_used)]

    # Build fanin/fanout/fan_locs (replicates C init logic exactly)
    fanin  = [[] for _ in range(total_used)]
    fanout = [[] for _ in range(total_used)]
    fan_locs = [[] for _ in range(total_used)]

    for net in nets:
        p = name_to_index[net['name']]
        for conn in net['connections']:
            f = name_to_index[conn]
            fanin[p].append(f)
            fanout[f].append(p)
            fan_locs[p].append(f)   # present gets fanin's loc
            fan_locs[f].append(p)   # fanin gets present's loc

    return name_to_index, total_used, all_names, fanin, fanout, fan_locs

def flatten(lists):
    """Flatten list-of-lists into (flat, offsets, counts)."""
    flat, offsets, counts = [], [], []
    off = 0
    for lst in lists:
        offsets.append(off)
        counts.append(len(lst))
        flat.extend(lst)
        off += len(lst)
    return flat, offsets, counts

def generate_assembly(file_path):
    """Generate RISC-V assembly with pre-computed index-based netlist data."""
    num_elements, max_x, max_y, nets = read_canneal_input(file_path)
    if num_elements is None:
        print("Error reading file: %s" % file_path, file=sys.stderr)
        sys.exit(1)

    _, total_used, all_names, fanin, fanout, fan_locs = build_index_data(nets)
    fanin_flat,    fanin_off,    fanin_cnt    = flatten(fanin)
    fanout_flat,   fanout_off,   fanout_cnt   = flatten(fanout)
    fanlocs_flat,  fanlocs_off,  fanlocs_cnt  = flatten(fan_locs)

    print("# Generated RISC-V assembly with pre-computed index-based netlist data")
    print("# From: %s" % file_path)
    print("# DO NOT EDIT: This file is auto-generated")
    print("# total_used = %d (named=%d, connection-only=%d)" %
          (total_used, num_elements, total_used - num_elements))
    print()
    print(".section .rodata, \"a\"")
    print()

    # === METADATA ===
    print("# === NETLIST METADATA ===")
    emit("compiled_num_elements", np.array([num_elements], dtype=np.uint64))
    print()
    emit("compiled_max_x", np.array([max_x], dtype=np.uint64))
    print()
    emit("compiled_max_y", np.array([max_y], dtype=np.uint64))
    print()
    emit("compiled_total_used", np.array([total_used], dtype=np.uint64))
    print()

    # === ELEMENT NAMES (for debugging / netlist_elem_from_name) ===
    print("# === ELEMENT NAMES ===")
    emit_string_array("compiled_element_names", all_names)
    print()

    # === FANIN ===
    print("# === FANIN INDEX DATA ===")
    emit("compiled_fanin_flat",    np.array(fanin_flat,  dtype=np.uint64) if fanin_flat else np.zeros(0, dtype=np.uint64))
    print()
    emit("compiled_fanin_offsets", np.array(fanin_off,   dtype=np.uint64))
    print()
    emit("compiled_fanin_counts",  np.array(fanin_cnt,   dtype=np.uint64))
    print()

    # === FANOUT ===
    print("# === FANOUT INDEX DATA ===")
    emit("compiled_fanout_flat",    np.array(fanout_flat,  dtype=np.uint64) if fanout_flat else np.zeros(0, dtype=np.uint64))
    print()
    emit("compiled_fanout_offsets", np.array(fanout_off,   dtype=np.uint64))
    print()
    emit("compiled_fanout_counts",  np.array(fanout_cnt,   dtype=np.uint64))
    print()

    # === FAN_LOCS ===
    print("# === FAN_LOCS INDEX DATA ===")
    emit("compiled_fanlocs_flat",    np.array(fanlocs_flat,  dtype=np.uint64) if fanlocs_flat else np.zeros(0, dtype=np.uint64))
    print()
    emit("compiled_fanlocs_offsets", np.array(fanlocs_off,   dtype=np.uint64))
    print()
    emit("compiled_fanlocs_counts",  np.array(fanlocs_cnt,   dtype=np.uint64))
    print()

if __name__ == "__main__":
    if len(sys.argv) < 2:
        print("Usage: %s <input_file>" % sys.argv[0], file=sys.stderr)
        sys.exit(1)
    input_file = sys.argv[1]

    num_elements, max_x, max_y, nets = read_canneal_input(input_file)
    if num_elements is None:
        print("Error reading file: %s" % input_file, file=sys.stderr)
        sys.exit(1)

    _, total_used, _, _, _, _ = build_index_data(nets)

    header_content = """// Auto-generated netlist sizes header
// DO NOT EDIT: This file is auto-generated
#ifndef NETLIST_SIZES_H
#define NETLIST_SIZES_H

#define NETLIST_MAX_ELEMENTS %d
#define NETLIST_MAX_X %d
#define NETLIST_MAX_Y %d
#define NETLIST_MAX_CHIP_SIZE (NETLIST_MAX_X * NETLIST_MAX_Y)
#define NETLIST_TOTAL_USED %d

#endif // NETLIST_SIZES_H
""" % (num_elements, max_x, max_y, total_used)

    with open('src/netlist_sizes.h', 'w') as f:
        f.write(header_content)

    generate_assembly(input_file)
