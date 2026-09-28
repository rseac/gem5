// v2: matches ubench_pf_mask_chain v2's structure (load once, repeat the
// timed op many times with no memory access in the loop) for a fair
// isolation of just the compare's contribution.
#include <stdint.h>
#include "printf.h"
#include "runtime.h"

#define REPS 64
static float arr[32] __attribute__((aligned(64)));
static float thresh[32] __attribute__((aligned(64)));

int main() {
    HW_CNT_READY;
    for (int i = 0; i < 32; i++) { arr[i] = (float)(i % 7); thresh[i] = 3.0f; }

    long gvl;
    asm volatile ("vsetvli %0, %1, e32, m1, ta, ma" : "=r"(gvl) : "r"(32));
    asm volatile ("vle32.v v1, (%0)" :: "r"(arr));
    asm volatile ("vle32.v v2, (%0)" :: "r"(thresh));

    start_timer();
    for (int r = 0; r < REPS; r++) {
        asm volatile ("vmflt.vv v0, v1, v2");     // compare -> mask v0
    }
    stop_timer();

    printf("[ROI-LATENCY]: %ld cycles\n", get_timer());
    printf("SUCCESS\n");
    return 0;
}
