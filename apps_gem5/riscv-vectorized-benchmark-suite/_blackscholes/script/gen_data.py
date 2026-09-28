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

def read_blackscholes_input(file_path):
    data = []
    with open(file_path, 'r') as f:
        lines = f.readlines()
        if not lines:
            return data
        numOptions = int(lines[0].strip())
        for line in lines[1:]:
            parts = line.strip().split()
            if len(parts) == 0:
                continue
            # Example: 42.00 40.00 0.1000 0.00 0.20 0.50 C 0.00 4.759423036851750055
            entry = {
                's': float(parts[0]),
                'strike': float(parts[1]),
                'r': float(parts[2]),
                'divq': float(parts[3]),
                'v': float(parts[4]),
                't': float(parts[5]),
                'option_type': parts[6],
                'divs': float(parts[7]),
                'DGrefval': float(parts[8])
            }
            data.append(entry)
    return numOptions, data

# Usage
if len(sys.argv) > 1:
  file_path = sys.argv[1]
else:
  file_path = "input/in_8.input"
numOptions, input_data = read_blackscholes_input(file_path)

# Create the file
print(".section .data,\"aw\",@progbits")
emit("numOptions", np.array(numOptions, dtype=np.uint32))
optype_list = []
s_arr = []
strike_arr = []
r_arr = []
divq_arr = []
v_arr = []
t_arr = []
divs_arr = []
DGrefval_arr = []
for entry in input_data:
    opt_type_val = 0 if entry['option_type'] == 'C' else 1
    optype_list.append(opt_type_val)
    s_arr.append(entry['s'])
    strike_arr.append(entry['strike'])
    r_arr.append(entry['r'])
    divq_arr.append(entry['divq'])
    v_arr.append(entry['v'])
    t_arr.append(entry['t'])
    divs_arr.append(entry['divs'])
    DGrefval_arr.append(entry['DGrefval'])
emit("sptprice", np.array(s_arr, dtype=np.float32), 'NR_LANES*4*NR_CLUSTERS')
emit("strike", np.array(strike_arr, dtype=np.float32), 'NR_LANES*4*NR_CLUSTERS')
emit("rate", np.array(r_arr, dtype=np.float32), 'NR_LANES*4*NR_CLUSTERS')
emit("divq", np.array(divq_arr, dtype=np.float32), 'NR_LANES*4*NR_CLUSTERS')
emit("volatility", np.array(v_arr, dtype=np.float32), 'NR_LANES*4*NR_CLUSTERS')
emit("otime", np.array(t_arr, dtype=np.float32), 'NR_LANES*4*NR_CLUSTERS')
emit("divs", np.array(divs_arr, dtype=np.float32), 'NR_LANES*4*NR_CLUSTERS')
emit("DGrefval", np.array(DGrefval_arr, dtype=np.float32), 'NR_LANES*4*NR_CLUSTERS')
emit("otype", np.array(optype_list, dtype=np.int32), 'NR_LANES*4*NR_CLUSTERS')
emit("prices", np.array([0.0] * numOptions, dtype=np.float32), 'NR_LANES*4*NR_CLUSTERS')
