// Isolated indexed-load (gather) calibration: repeat vluxei64.v alone
// (fixed, precomputed byte-offset indices, e64) many times, sweeping vl,
// to directly measure the real cost of the gather instruction itself --
// separate from spmv's real usage (ubench_spmv_gather), which mixes it
// with a shift and an accumulate and can't be cleanly decomposed by
// subtraction. computeGather()'s current model (LOW confidence) is
// T = c_harness + max(15 + vl*c_per_elem, c_startup_floor), with
// c_per_elem/c_startup_floor both currently assumed lane-dependent.
#include <stdint.h>
#include "printf.h"
#include "runtime.h"

#define REPS 32
static double x[256] __attribute__((aligned(64)));
static uint64_t offs[16] __attribute__((aligned(64)));

static void gather_chain(int64_t vl) {
    asm volatile("vsetvli zero, %0, e64, m1, ta, ma" : : "r"(vl));
    asm volatile("vle64.v v3, (%0)" :: "r"(offs));  // fixed byte offsets, loaded once
    start_timer();
    for (int r = 0; r < REPS; r++) {
        asm volatile("vluxei64.v v4, (%0), v3" :: "r"(x));
    }
    stop_timer();
    printf("[VL]: %ld [ROI-LATENCY]: %ld cycles\n", vl, get_timer());
}

int main() {
    HW_CNT_READY;
    for (int i = 0; i < 16; i++) offs[i] = (uint64_t)((i % 8) * 8);
    for (int i = 0; i < 256; i++) x[i] = (double)i;

    static const int64_t vls[] = {2, 4, 8, 16, 32};
    for (unsigned i = 0; i < sizeof(vls) / sizeof(vls[0]); i++) {
        gather_chain(vls[i]);
    }
    printf("SUCCESS\n");
    return 0;
}
