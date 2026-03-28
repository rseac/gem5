
CC=/riscv/_install/bin/riscv64-unknown-elf-gcc -std=c99
CXX=/riscv/_install/bin/riscv64-unknown-elf-g++
OBJDUMP=/riscv/_install/bin/riscv64-unknown-elf-objdump
FC=gfortran
flags = -O3 -fstrict-aliasing -fivopts -flax-vector-conversions -march=rv64gcv_zfh_zvfh -mabi=lp64d -Wall
vecflags = -ftree-vectorize
noscalvecflags = -ftree-vectorize
novecflags = -fno-tree-vectorize
omp_flags=-fopenmp

# gem5 m5ops support (build libm5 first: cd util/m5 && scons build/riscv/out/m5)
# Usage: make USE_M5OPS=1
GEM5_ROOT ?= $(abspath $(dir $(lastword $(MAKEFILE_LIST)))/../..)
M5OPS_INCLUDE ?= $(GEM5_ROOT)/include
M5OPS_LIB_DIR ?= $(GEM5_ROOT)/util/m5/build/riscv/out
ifdef USE_M5OPS
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

