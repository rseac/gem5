# Copyright 2020-2025 ETH Zurich and University of Bologna.
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

# Author: Samuel Riedel, ETH Zurich
#         Matheus Cavalcante, ETH Zurich
SHELL = /usr/bin/env bash

ROOT_DIR := $(patsubst %/,%, $(dir $(abspath $(lastword $(MAKEFILE_LIST)))))
ARA_DIR := /home/twiga/code/github/ai/my-timing-project/benchmark_suite/ara
ifeq ($(strip $(ARA_DIR)),)
ARA_DIR := $(abspath $(ROOT_DIR)/../..)
endif

# Choose Ara's configuration
ifndef config
	ifdef ARA_CONFIGURATION
		config := $(ARA_CONFIGURATION)
	else
		config := default
	endif
endif

# Include configuration
include $(ARA_DIR)/config/$(config).mk

INSTALL_DIR             ?= $(ARA_DIR)/install
GCC_INSTALL_DIR         ?= $(INSTALL_DIR)/riscv-gcc
LLVM_INSTALL_DIR        ?= $(INSTALL_DIR)/riscv-llvm
ISA_SIM_INSTALL_DIR     ?= $(INSTALL_DIR)/riscv-isa-sim
ISA_SIM_MOD_INSTALL_DIR ?= $(INSTALL_DIR)/riscv-isa-sim-mod

RISCV_XLEN    ?= 64
RISCV_ARCH    ?= rv$(RISCV_XLEN)gcv
RISCV_ABI     ?= lp64d
RISCV_TARGET  ?= riscv$(RISCV_XLEN)-unknown-elf

# Use GCC toolchain
RISCV_PREFIX  ?= /riscv/_install/bin/riscv64-unknown-elf-
RISCV_CC      ?= $(RISCV_PREFIX)gcc
RISCV_CXX     ?= $(RISCV_PREFIX)g++
RISCV_OBJDUMP ?= $(RISCV_PREFIX)objdump
RISCV_OBJCOPY ?= $(RISCV_PREFIX)objcopy
RISCV_AS      ?= $(RISCV_PREFIX)as
RISCV_AR      ?= $(RISCV_PREFIX)ar
RISCV_LD      ?= $(RISCV_PREFIX)ld
RISCV_STRIP   ?= $(RISCV_PREFIX)strip

# Use gcc to compile scalar riscv-tests
RISCV_CC_GCC  ?= $(RISCV_PREFIX)gcc

# Benchmark with spike
spike_env_dir ?= $(ARA_DIR)/apps/riscv-tests
SPIKE_INC     ?= -I$(spike_env_dir)/env -I$(spike_env_dir)/benchmarks/common
SPIKE_CCFLAGS ?= -DPREALLOCATE=1 -DSPIKE=1 $(SPIKE_INC)
SPIKE_LDFLAGS ?= -nostdlib -T$(spike_env_dir)/benchmarks/common/test.ld
RISCV_SIM     ?= $(ISA_SIM_INSTALL_DIR)/bin/spike
RISCV_SIM_MOD ?= $(ISA_SIM_MOD_INSTALL_DIR)/bin/spike
# VLEN should be lower or equal than 4096 because of spike restrictions
vlen_spike := $(shell vlen=$$(grep vlen $(ARA_DIR)/config/$(config).mk | cut -d" " -f3) && echo "$$(( $$vlen < 4096 ? $$vlen : 4096 ))")
RISCV_SIM_OPT ?= --isa=rv64gcv_zfh --varch="vlen:$(vlen_spike),elen:64"
RISCV_SIM_MOD_OPT ?= --isa=rv64gcv_zfh --varch="vlen:$(vlen_spike),elen:64" -d

# Python
PYTHON ?= python3

# Defines
ENV_DEFINES ?=
ifeq ($(vcd_dump),1)
ENV_DEFINES += -DVCD_DUMP=1
endif
MAKE_DEFINES = -DNR_LANES=$(nr_lanes) -DVLEN=$(vlen) -DNR_CLUSTERS=$(nr_clusters)
DEFINES += $(ENV_DEFINES) $(MAKE_DEFINES)

# Common flags
RISCV_WARNINGS += -Wunused-variable -Wall -Wextra -Wno-unused-command-line-argument # -Werror

# Compiler Flags
LLVM_FLAGS     ?= -march=rv64gcv -mabi=$(RISCV_ABI)
RISCV_FLAGS    ?= $(LLVM_FLAGS) -mcmodel=medany -I$(CURDIR)/common -O3 -ffast-math -fno-common -fno-builtin-printf -fno-builtin-memset -fno-builtin-memcpy -fno-tree-loop-distribute-patterns -fno-tree-vectorize $(DEFINES) $(RISCV_WARNINGS)
RISCV_CCFLAGS  ?= $(RISCV_FLAGS) -ffunction-sections -fdata-sections -std=gnu99
RISCV_CCFLAGS_SPIKE  ?= $(RISCV_FLAGS) $(SPIKE_CCFLAGS) -ffunction-sections -fdata-sections -std=gnu99
RISCV_CXXFLAGS ?= $(RISCV_FLAGS) -ffunction-sections -fdata-sections
RISCV_LDFLAGS  ?= -static -nostartfiles -lm -Wl,--gc-sections
RISCV_LDFLAGS_SPIKE  ?= -static -nostartfiles -lm $(SPIKE_LDFLAGS) -Wl,--gc-sections

# GCC Flags
RISCV_FLAGS_GCC    ?= -mcmodel=medany -march=$(RISCV_ARCH) -mabi=$(RISCV_ABI) -I$(CURDIR)/common -static -std=gnu99 -O3 -ffast-math -fno-common -fno-builtin-printf -fno-builtin-memset -fno-builtin-memcpy -fno-tree-loop-distribute-patterns -fno-tree-vectorize $(DEFINES) $(RISCV_WARNINGS)
RISCV_CCFLAGS_GCC  ?= $(RISCV_FLAGS_GCC)
RISCV_CXXFLAGS_GCC ?= $(RISCV_FLAGS_GCC)
RISCV_LDFLAGS_GCC  ?= -static -nostartfiles -lm -lgcc $(RISCV_FLAGS_GCC)

RISCV_OBJDUMP_FLAGS ?=

# Compile two different versions of the runtime, since we cannot link code compiled with two different toolchains
RUNTIME_GCC   ?= common/crt0-gcc.S.o common/printf-gcc.c.o common/string-gcc.c.o common/serial-gcc.c.o common/util-gcc.c.o common/syscalls-gcc.c.o
RUNTIME_LLVM  ?= common/crt0-llvm.S.o common/printf-llvm.c.o common/string-llvm.c.o common/serial-llvm.c.o common/util-llvm.c.o common/syscalls-llvm.c.o
RUNTIME_SPIKE ?= $(spike_env_dir)/benchmarks/common/crt.S.o.spike $(spike_env_dir)/benchmarks/common/syscalls.c.o.spike common/util.c.o.spike

.INTERMEDIATE: $(RUNTIME_GCC) $(RUNTIME_LLVM)

common/printf-gcc.c.o: common/printf.c
	$(RISCV_CC_GCC) $(RISCV_CCFLAGS_GCC) -fno-tree-vectorize -c $< -o $@

common/printf-llvm.c.o: common/printf.c
	$(RISCV_CC) $(RISCV_CCFLAGS) -fno-tree-vectorize -c $< -o $@

%-gcc.S.o: %.S
	$(RISCV_CC_GCC) $(RISCV_CCFLAGS_GCC) -c $< -o $@

%-gcc.c.o: %.c
	$(RISCV_CC_GCC) $(RISCV_CCFLAGS_GCC) -c $< -o $@

%-llvm.S.o: %.S
	$(RISCV_CC) $(RISCV_CCFLAGS) -c $< -o $@

%-llvm.c.o: %.c
	$(RISCV_CC) $(RISCV_CCFLAGS) -c $< -o $@

%.S.o: %.S
	$(RISCV_CC) $(RISCV_CCFLAGS) -c $< -o $@

%.c.o: %.c
	$(RISCV_CC) -I$(dir $<) $(RISCV_CCFLAGS) -c $< -o $@

%.S.o.spike: %.S patch-spike-crt0
	$(RISCV_CC) $(RISCV_CCFLAGS_SPIKE) -c $< -o $@

%.c.o.spike: %.c
	$(RISCV_CC) -I$(dir $<) $(RISCV_CCFLAGS_SPIKE) -c $< -o $@

%.cpp.o: %.cpp
	$(RISCV_CXX) -I$(dir $<) $(RISCV_CXXFLAGS) -c $< -o $@

%.ld: %.ld.c
	$(RISCV_CC) -P -E $(DEFINES) $< -o $@
