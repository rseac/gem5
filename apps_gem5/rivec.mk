# Copyright 2026 ETH Zurich and University of Bologna.
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

# Author: Navaneeth Kunhi Purayil, ETH Zurich (nkunhi@iis.ee.ethz.ch)

RIVEC_PATH := riscv-vectorized-benchmark-suite
RIVEC_DIR := $(APPS_DIR)/$(RIVEC_PATH)
RIVEC_APPS := _axpy _blackscholes _pathfinder _streamcluster _spmv _canneal _swaptions _lavaMD _matmul _jacobi-2d _particlefilter _somier
RIVEC_BINARIES := $(addprefix bin/, $(RIVEC_APPS))

rivec_binaries: $(RIVEC_BINARIES)

# RiVEC SpMV dataset path (used by riscv-vectorized-benchmark-suite/_spmv)
def_args__axpy           ?= "1024"
def_args__blackscholes   ?= "input/in_8.input"
def_args__pathfinder     ?= "input/data_small.in"
def_args__streamcluster  ?= "3 3 128 8 8 10"
def_args__spmv           ?= "input/football.mtx"
def_args__canneal        ?= "input/10.nets"
def_args__swaptions      ?= "128 1"
def_args__lavaMD         ?= "1 1 32"

# _matmul, _somier and _particlefilter have no argv/filesystem support on the
# bare-metal RTL testbench (crt0.S enters main() with argc=0, argv=NULL), so
# they use compile-time fixed-size builds instead of the def_args_* mechanism
# above. Sizes below match each benchmark's existing "tiny" RiVEC dataset.
bin/_matmul:          ENV_DEFINES += -DMATMUL_SIZE=64
bin/_somier:          ENV_DEFINES += -DSOMIER_NTSTEPS=5 -DSOMIER_N=10
bin/_particlefilter:  ENV_DEFINES += -DPARTICLEFILTER_ISZX=128 -DPARTICLEFILTER_ISZY=128 \
                                      -DPARTICLEFILTER_NFR=2 -DPARTICLEFILTER_NP=256

# Adding compile flags for RiVEC

RIVEC_INCLUDES := -I$(RIVEC_DIR) $(shell find $(RIVEC_DIR) -maxdepth 2 -type d -exec echo -I{} \;)
RISCV_CCFLAGS += $(RIVEC_INCLUDES) -DUSE_RISCV_VECTOR
RISCV_CXXFLAGS += $(RIVEC_INCLUDES) -DUSE_RISCV_VECTOR

define rivec_gen_data_template
.PHONY: $1/data.S
$1/data.S:
	cd $1 && if [ -d script ]; then ${PYTHON} script/gen_data.py $(strip $(subst ",,$(or $(def_args_$(notdir $1)),$(def_args_$(patsubst _%,%,$(notdir $1)))))) > data.S ; else touch data.S; fi
endef
$(foreach app,$(RIVEC_APPS),$(eval $(call rivec_gen_data_template, $(RIVEC_PATH)/$(app))))

define rivec_compile_template
bin/$1: $(RIVEC_PATH)/$1/data.S.o $(addsuffix .o, $(shell find $(RIVEC_DIR)/$(1) -name "*.c" -o -name "*.S" -o -name "*.cpp")) $(RUNTIME_LLVM) linker_script
	mkdir -p bin/
	$$(RISCV_CC) $$(RISCV_CCFLAGS) -o $$@ $$(addsuffix .o, $$(shell find $(RIVEC_DIR)/$(1) -name "*.c" -o -name "*.S" -o -name "*.cpp")) $(RUNTIME_LLVM) $$(RISCV_LDFLAGS) -T$$(CURDIR)/common/link.ld
	$$(RISCV_OBJDUMP) $$(RISCV_OBJDUMP_FLAGS) -D $$@ > $$@.dump
	$$(RISCV_STRIP) $$@ -S --strip-unneeded
endef
$(foreach app,$(RIVEC_APPS),$(eval $(call rivec_compile_template,$(app))))
