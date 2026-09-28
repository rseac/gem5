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

import random as rand
import numpy as np
import sys

def emit(name, array, alignment='8'):
  print(".global %s" % name)
  print(".balign " + alignment)
  print("%s:" % name)
  bs = array.tobytes()
  for i in range(0, len(bs), 4):
    s = ""
    for n in range(4):
      s += "%02x" % bs[i+3-n]
    print("    .word 0x%s" % s)

def read_pathfinder_input(file_path):
    """Read pathfinder input file with matrix dimensions, data, and reference."""
    wall = []
    reference = []
    with open(file_path, 'r') as f:
        lines = f.readlines()
        if not lines:
            return None, None, None, None
        # First line contains dimensions: rows cols
        dims = lines[0].strip().split()
        rows = int(dims[0])
        cols = int(dims[1])
        
        # Read the matrix data (rows lines of cols values each)
        for line in lines[1:rows+1]:
            parts = line.strip().split()
            if len(parts) == 0:
                continue
            # Convert to integers and add to wall matrix
            wall.extend([int(x) for x in parts])
        
        # Read the reference data (after empty line)
        if len(lines) > rows + 2:
            ref_parts = lines[rows + 2].strip().split()
            reference = [int(x) for x in ref_parts]
    
    return rows, cols, wall, reference

if len(sys.argv) > 1:
  file_path = sys.argv[1]
else:
  file_path = "input/data_small.in"

rows, cols, wall_data, reference_data = read_pathfinder_input(file_path)

# Create the file
print(".section .data,\"aw\",@progbits")
emit("rows", np.array(rows, dtype=np.uint32))
emit("cols", np.array(cols, dtype=np.uint32))
emit("wall", np.array(wall_data, dtype=np.int32), 'NR_LANES*4*NR_CLUSTERS')
emit("result", np.array([0] * cols, dtype=np.int32), 'NR_LANES*4*NR_CLUSTERS')
emit("reference", np.array(reference_data, dtype=np.int32), 'NR_LANES*4*NR_CLUSTERS')
