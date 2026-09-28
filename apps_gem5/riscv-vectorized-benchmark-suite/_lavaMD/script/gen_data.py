#!/usr/bin/env python3
# Copyright 2022-2025 ETH Zurich and University of Bologna.
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

if len(sys.argv) > 3:
    cores = int(sys.argv[1])
    boxes1d = int(sys.argv[2])
    NUMBER_PAR_PER_BOX = int(sys.argv[3])
else:
    cores = 1
    boxes1d = 1
    NUMBER_PAR_PER_BOX = 32

emit("cores", np.array(cores, dtype=np.uint32))
emit("boxes1d", np.array(boxes1d, dtype=np.uint32))
emit("NUMBER_PAR_PER_BOX", np.array(NUMBER_PAR_PER_BOX, dtype=np.uint32))


