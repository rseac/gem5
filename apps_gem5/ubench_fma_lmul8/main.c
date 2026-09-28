// Calibration probe: validates Ara's computeFPFMA() throughput term
// (n_beats, from computeNBeats) at LMUL=8, SEW=64 -- the EXACT pattern
// real axpy uses (confirmed via riscv-vectorized-benchmark-suite/_axpy/
// src/axpy.c's inline-asm path: vsetvli e64,m8 -> vle64 x2 -> vfmacc.vf
// -> vse64). None of axpy/streamcluster/lavamd's FMA traffic hits Ara's
// LMUL=1 lookup table (confirmed via VPPP_TIMING_DEBUG: all "other_lmul",
// zero "lmul1_count") -- it falls through to the general formula
// max(n_beats, lat_fp) + c_fe_fpu, where n_beats dominates at this scale
// and was never independently RTL-validated for the FPU specifically
// (only the memory path's n_beats-driven bandwidth was recalibrated this
// session). axpy's FMA is inherently independent across iterations
// (fresh dx/dy loaded each time), so this is a genuine throughput probe.
#include <stdint.h>
#include "printf.h"
#include "runtime.h"

#define N 2048
static double dx[N] __attribute__((aligned(64)));
static double dy[N] __attribute__((aligned(64)));

int main() {
    HW_CNT_READY;
    for (int i = 0; i < N; i++) { dx[i] = 1.0; dy[i] = 2.0; }
    double a = 3.0;

    start_timer();
    long i = 0;
    long gvl;
    while (i < N) {
        asm volatile ("vsetvli %0, %1, e64, m8, ta, ma" : "=r"(gvl) : "r"(N - i));
        asm volatile ("vle64.v v8, (%0)" :: "r"(&dx[i]));
        asm volatile ("vle64.v v16, (%0)" :: "r"(&dy[i]));
        asm volatile ("vfmacc.vf v16, %0, v8" :: "f"(a));
        asm volatile ("vse64.v v16, (%0)" :: "r"(&dy[i]));
        i += gvl;
    }
    stop_timer();

    printf("[N]: %d [ROI-LATENCY]: %ld cycles\n", N, get_timer());
    printf("SUCCESS\n");
    return 0;
}
