
CC=/riscv/_install/bin/riscv64-unknown-elf-gcc -std=c99
CXX=/riscv/_install/bin/riscv64-unknown-elf-g++
OBJDUMP=/riscv/_install/bin/riscv64-unknown-elf-objdump
FC=gfortran
flags = -mcmodel=medany -march=rv64gcv_zifencei -mabi=lp64d -I/ara/apps/common -static -O3 -fno-common -fno-builtin-printf -DNR_LANES=$(nr_lanes) -DVLEN=$(vlen) -ffunction-sections -fdata-sections -std=gnu99
vecflags = -ftree-vectorize
noscalvecflags = -ftree-vectorize
novecflags = -fno-tree-vectorize
omp_flags=-fopenmp

# Print cycle count immediately at the end of each ROI (default: disabled)
# enable with: make RDCYCLE=1
ifdef RDCYCLE
flags += -DRDCYCLE
endif

# gem5 m5ops ROI support — enabled by default, disable with: make NO_M5OPS=1
# (build libm5 first: cd util/m5 && scons build/riscv/out/m5)
GEM5_ROOT ?= $(abspath $(dir $(lastword $(MAKEFILE_LIST)))/../..)
M5OPS_INCLUDE ?= $(GEM5_ROOT)/include
M5OPS_LIB_DIR ?= $(GEM5_ROOT)/util/m5/build/riscv/out
ifndef NO_M5OPS
flags += -DUSE_M5OPS -I$(M5OPS_INCLUDE)
m5ops_libs = -L$(M5OPS_LIB_DIR) -lm5
endif

# Compile-time kernel whitelist: make KERNELS="s000 s111 va"
# Produces a binary that only runs the listed kernels; omit for all kernels.
ifdef KERNELS
flags += -DTSVC_KERNELS='"$(KERNELS)"'
endif

ifdef VEC_REPORT
vecflags+=-ftree-vectorizer-verbose=1 -fopt-info-vec=gcc.vect -fopt-info-vec-missed=gcc.miss -fno-inline
endif

ieee_math_flags=
fast_math_flags=-ffast-math

ifdef PRECISE_MATH
$(warning No 'precise' math flags for GNU!)
endif

