DEBUG_FLAGS ?= Cache,CacheVerbose,ExecEnable,ExecUser,ExecMacro,ExecMicro,MMU
FLAGS := --enable-chaining --vlen 512 --vector-timing-throughput 4 --simd-units 2
OUTDIR ?= m5out
PROG_DIR ?= ./tests/test-progs/custom/bin/
PROG ?= stride1
UC ?= 0
VC ?= 0

.PHONY: build verify
build:
	build/RISCV/gem5.opt --debug-flags=$(DEBUG_FLAGS) --outdir=m5out --debug-file=exec.log rvv/riscv-rvv-se-ara-prefetcher.py $(FLAGS) $(PROG_DIR)/$(PROG)
ifeq ($(UC), 1)
	build/RISCV/gem5.opt --debug-flags=$(DEBUG_FLAGS) --outdir=m5out2 --debug-file=exec.log rvv/riscv-rvv-se-ara-prefetcher.py $(FLAGS) --scalar-uncacheable $(PROG_DIR)/$(PROG)
endif
ifeq ($(VC), 1)
	build/RISCV/gem5.opt --debug-flags=$(DEBUG_FLAGS),VectorSplitter --outdir=m5out2 --debug-file=exec.log rvv/riscv-rvv-se-ara-prefetcher.py $(FLAGS) --vector-cache $(PROG_DIR)/$(PROG)
endif
verify:
	cmp m5out/$(PROG)_result.bin m5out2/$(PROG)_result.bin && echo PASS