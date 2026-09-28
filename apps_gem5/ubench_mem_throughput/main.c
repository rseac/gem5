// Independent-Register Unit-Stride Memory Calibration Microbenchmark (RTL)
//
// Measures real Ara RTL cost of N=32 independent vle64.v / vse64.v
// instructions (rotating v0/v8/v16/v24, no register reuse) at a swept vl, so
// the fixed per-instruction issue cost and the per-element streaming rate
// can be separated by linear regression: T(vl) = a + b*vl.
//
// Register choice: at LMUL=8 (m8), RVV requires destination vector register
// groups to be 8-aligned, so only v0/v8/v16/v24 are valid independent m8
// group starts (unlike LMUL=1, which allows any of v8..v15 individually).
//
// This is the ground-truth counterpart to VP++'s computeUnitLoad/Store,
// which currently uses a single hardcoded tau_mem=10 "round trip" constant
// (see simulators/vppp_ara_timing_model/vp/src/core/common/ara_timing.cpp)
// with no real AXI/interconnect measurement behind it for this project.
//
// Prints one [ROI-LATENCY]/[VL] pair per swept vl value in a single run, to
// minimize the number of (slow) RTL Verilator invocations needed.
#include <stdint.h>
#include "printf.h"
#include "runtime.h"

#define MEM_BUF_ELEMS 2048
static double g_buf[MEM_BUF_ELEMS] __attribute__((aligned(4 * NR_LANES * NR_CLUSTERS)));

static void ubench_mem_load_throughput(int64_t vl) {
    asm volatile("vsetvli zero, %0, e64, m8, ta, ma" : : "r"(vl));
    double* p = g_buf;
    start_timer();
    for (int rep = 0; rep < 8; rep++) {
        asm volatile("vle64.v v0,  (%0)" :: "r"(p));
        asm volatile("vle64.v v8,  (%0)" :: "r"(p));
        asm volatile("vle64.v v16, (%0)" :: "r"(p));
        asm volatile("vle64.v v24, (%0)" :: "r"(p));
    }
    stop_timer();
    printf("[VL]: %ld [OP]: load_throughput [ROI-LATENCY]: %ld cycles\n", vl, get_timer());
}

static void ubench_mem_store_throughput(int64_t vl) {
    asm volatile("vsetvli zero, %0, e64, m8, ta, ma" : : "r"(vl));
    double* p = g_buf;
    start_timer();
    for (int rep = 0; rep < 8; rep++) {
        asm volatile("vse64.v v0,  (%0)" :: "r"(p));
        asm volatile("vse64.v v8,  (%0)" :: "r"(p));
        asm volatile("vse64.v v16, (%0)" :: "r"(p));
        asm volatile("vse64.v v24, (%0)" :: "r"(p));
    }
    stop_timer();
    printf("[VL]: %ld [OP]: store_throughput [ROI-LATENCY]: %ld cycles\n", vl, get_timer());
}

int main() {
    HW_CNT_READY;
    printf("=== ubench_mem_throughput (NR_LANES=%d VLEN=%d) ===\n", NR_LANES, VLEN);
    static const int64_t vls[] = {16, 32, 64, 128, 256, 512, 1024};
    for (unsigned i = 0; i < sizeof(vls) / sizeof(vls[0]); i++) {
        if (vls[i] > MEM_BUF_ELEMS) continue;
        ubench_mem_load_throughput(vls[i]);
        ubench_mem_store_throughput(vls[i]);
    }
    printf("SUCCESS\n");
    return 0;
}
