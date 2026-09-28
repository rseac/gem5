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

# create a matrix of chunksize * dim elements
if len(sys.argv) > 6:
  kmin = int(sys.argv[1])
  kmax = int(sys.argv[2])
  dim = int(sys.argv[3])
  n = int(sys.argv[4])
  chunksize = int(sys.argv[5])
  clustersize = int(sys.argv[6])
else :
  # Min & Max number of clusters
  kmin = 3
  kmax = 3

  # Dimension of each datapaoint and number of datapoints
  dim = 128
  n = 8

  # Chunks of datapoints streamed, for now same as numer of datapoints itself
  chunksize = 8

  # To hold the centers, should be greated than kmax
  clustersize = 10

block = np.random.rand(chunksize * dim).astype(np.float32)

# Create the file
print(".section .data,\"aw\",@progbits")

# Parameters
emit("KMIN", np.array(kmin, dtype=np.int32))
emit("KMAX", np.array(kmax, dtype=np.int32))
emit("DIM", np.array(dim, dtype=np.int32))
emit("N", np.array(n, dtype=np.int32))
emit("CHUNKSIZE", np.array(chunksize, dtype=np.int32))
emit("CLUSTERSIZE", np.array(clustersize, dtype=np.int32))

# Data
emit("block", block, 'NR_LANES*4*NR_CLUSTERS')