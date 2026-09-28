// Register-Reuse ("Hazard-Exposed") Unit-Stride Memory Calibration
// Microbenchmark (RTL)
//
// Same idea as ubench_mem_throughput, but every iteration reuses the SAME
// destination register (v8), matching how real compiled RiVEC kernels like
// axpy actually issue unit-stride loads/stores in a tight loop (see
// riscv-vectorized-benchmark-suite/_axpy/src/axpy.c: `vle64.v v8,(%0)` /
// `vle64.v v16,(%0)` reused every iteration). This is the case that's
// actually exposed on the critical path in real benchmarks, so it should be
// weighted more heavily than ubench_mem_throughput when refitting VP++'s
// tau_mem / n_beats memory-cost constants.
#include <stdint.h>
#include "printf.h"
#include "runtime.h"

#define MEM_BUF_ELEMS 2048
static double g_buf[MEM_BUF_ELEMS] __attribute__((aligned(4 * NR_LANES * NR_CLUSTERS)));

static void ubench_mem_load_hazards(int64_t vl) {
    asm volatile("vsetvli zero, %0, e64, m8, ta, ma" : : "r"(vl));
    double* p = g_buf;
    start_timer();
    for (int i = 0; i < 32; i++) {
        asm volatile("vle64.v v8, (%0)" :: "r"(p));
    }
    stop_timer();
    printf("[VL]: %ld [OP]: load_hazards [ROI-LATENCY]: %ld cycles\n", vl, get_timer());
}

static void ubench_mem_store_hazards(int64_t vl) {
    asm volatile("vsetvli zero, %0, e64, m8, ta, ma" : : "r"(vl));
    double* p = g_buf;
    start_timer();
    for (int i = 0; i < 32; i++) {
        asm volatile("vse64.v v8, (%0)" :: "r"(p));
    }
    stop_timer();
    printf("[VL]: %ld [OP]: store_hazards [ROI-LATENCY]: %ld cycles\n", vl, get_timer());
}

int main() {
    HW_CNT_READY;
    printf("=== ubench_mem_hazards (NR_LANES=%d VLEN=%d) ===\n", NR_LANES, VLEN);
    static const int64_t vls[] = {16, 32, 64, 128, 256, 512, 1024};
    for (unsigned i = 0; i < sizeof(vls) / sizeof(vls[0]); i++) {
        if (vls[i] > MEM_BUF_ELEMS) continue;
        ubench_mem_load_hazards(vls[i]);
        ubench_mem_store_hazards(vls[i]);
    }
    printf("SUCCESS\n");
    return 0;
}
