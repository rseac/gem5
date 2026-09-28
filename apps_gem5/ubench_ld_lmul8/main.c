// Isolation probe: same LMUL=8/SEW=64/vl=128 regime as ubench_fma_lmul8,
// but LOAD ONLY (no FMA, no store) -- to determine whether the -19.5%
// gap found in ubench_fma_lmul8 comes from the shared computeNBeats()
// term (which the memory path already uses) or specifically from the
// FPU/FMA path (max(n_beats, lat_fp) + c_fe_fpu).
#include <stdint.h>
#include "printf.h"
#include "runtime.h"

#define N 2048
static double dx[N] __attribute__((aligned(64)));

int main() {
    HW_CNT_READY;
    for (int i = 0; i < N; i++) dx[i] = 1.0;

    start_timer();
    long i = 0;
    long gvl;
    while (i < N) {
        asm volatile ("vsetvli %0, %1, e64, m8, ta, ma" : "=r"(gvl) : "r"(N - i));
        asm volatile ("vle64.v v8, (%0)" :: "r"(&dx[i]));
        i += gvl;
    }
    stop_timer();

    printf("[N]: %d [ROI-LATENCY]: %ld cycles\n", N, get_timer());
    printf("SUCCESS\n");
    return 0;
}
