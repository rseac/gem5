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
#
# Reads a sparse matrix in MTX format (col row value, sorted by row) and
# emits assembly data for the CSR arrays ia, ja, a, x, y — mirroring the
# parsing logic in src/main.c.  Optionally reads a .verif file and emits
# the expected result vector.
#
# Usage: gen_data.py [<file.mtx> [<file.verif>]]

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

def read_mtx(filename):
  """Read MTX file and build CSR arrays, replicating the logic of main.c."""
  with open(filename, 'r') as f:
    # Skip comment lines starting with '%'
    for line in f:
      if not line.startswith('%'):
        parts = line.split()
        M, N, NZ = int(parts[0]), int(parts[1]), int(parts[2])
        break

    ia = np.zeros(M + 1, dtype=np.uint64)
    ja = np.zeros(NZ,     dtype=np.uint64)
    a  = np.zeros(NZ,     dtype=np.float64)

    ia[0] = 0
    currentRow = 0

    for i in range(NZ):
      parts = f.readline().split()
      col   = int(parts[0])
      row   = int(parts[1])
      value = float(parts[2])

      ja[i] = col
      a[i]  = value

      if row != currentRow:
        diff = row - currentRow
        for j in range(diff):
          currentRow += 1
          ia[currentRow] = i

    if currentRow < M - 1:
      diff = M - currentRow
      for j in range(diff):
        currentRow += 1
        ia[currentRow] = NZ

    ia[M] = NZ

  x = np.ones(N,  dtype=np.float64)
  y = np.zeros(M, dtype=np.float64)

  return M, N, NZ, ia, ja, a, x, y

def read_verif(filename, M):
  """Read .verif file with comma-separated values across one or many lines."""
  values = []
  with open(filename, 'r') as f:
    for line in f:
      for token in line.split(','):
        token = token.strip()
        if not token:
          continue
        values.append(float(token))

  if len(values) < M:
    raise ValueError("Unexpected end of verif file: expected %d values, got %d" % (M, len(values)))
  if len(values) > M:
    raise ValueError("Too many values in verif file: expected %d, got %d" % (M, len(values)))

  return np.array(values, dtype=np.float64)

############
## SCRIPT ##
############

if len(sys.argv) > 1:
  input_file = sys.argv[1]
else:
  input_file = "input/lhr07.mtx"

if len(sys.argv) > 2:
  verif_file = sys.argv[2]
else:
  verif_file = input_file.replace(".mtx", ".verif")

M, N, NZ, ia, ja, a, x, y = read_mtx(input_file)

# Calculate non-zeros per row and the maximum/minimum
nnz_per_row = np.zeros(M, dtype=np.uint64)
for i in range(M):
  nnz_per_row[i] = ia[i+1] - ia[i]
max_nnz_per_row = np.max(nnz_per_row)
min_nnz_per_row = np.min(nnz_per_row[nnz_per_row > 0])

print(".section .data,\"aw\",@progbits")
emit("M",  np.array(M,  dtype=np.uint64))
emit("N",  np.array(N,  dtype=np.uint64))
emit("NZ", np.array(NZ, dtype=np.uint64))
emit("max_nnz_per_row", np.array(max_nnz_per_row, dtype=np.uint64))
emit("min_nnz_per_row", np.array(min_nnz_per_row, dtype=np.uint64))
emit("ia", ia, 'NR_LANES*4*NR_CLUSTERS')
emit("ja", ja, 'NR_LANES*4*NR_CLUSTERS')
emit("a",  a,  'NR_LANES*4*NR_CLUSTERS')
emit("nnz_per_row", nnz_per_row, 'NR_LANES*4*NR_CLUSTERS')
emit("x",  x,  'NR_LANES*4*NR_CLUSTERS')
emit("y",  y,  'NR_LANES*4*NR_CLUSTERS')

verif = read_verif(verif_file, M)
emit("verif", verif, 'NR_LANES*4*NR_CLUSTERS')

