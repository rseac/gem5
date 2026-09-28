// Independent-register vfmacc.vf throughput at LMUL=1/SEW=32, vl=32 -- the
// exact shape (FU, SEW, LMUL, VL) that Ara's RTL calibration lookup table
// (lookupRTL(), ara_timing.cpp) hits for lavamd's kernel_vec accumulation
// step. lookupRTL() currently returns the SAME value for both dependent-
// chain latency and structural FU occupancy; this probe measures the real
// occupancy (back-to-back INDEPENDENT accumulators, no register reuse) to
// check whether that conflation is inflating lavamd's estimate the same
// way it inflated independent-register LSU loads before that fix.
#include <stdint.h>
#include "printf.h"
#include "runtime.h"

#define REPS 32
static float a[32] __attribute__((aligned(64)));
static float b[32] __attribute__((aligned(64)));

int main() {
    HW_CNT_READY;
    for (int i = 0; i < 32; i++) { a[i] = (float)(i % 5) + 1.0f; b[i] = (float)(i % 3) + 1.0f; }

    long gvl;
    asm volatile ("vsetvli %0, %1, e32, m1, ta, ma" : "=r"(gvl) : "r"(32));
    asm volatile ("vle32.v v1, (%0)" :: "r"(a));
    asm volatile ("vle32.v v2, (%0)" :: "r"(b));
    asm volatile ("vle32.v v3, (%0)" :: "r"(a));
    asm volatile ("vle32.v v4, (%0)" :: "r"(a));
    asm volatile ("vle32.v v5, (%0)" :: "r"(a));
    asm volatile ("vle32.v v6, (%0)" :: "r"(a));

    start_timer();
    for (int r = 0; r < REPS; r++) {
        // 4 independent accumulators, matching lavamd's xfA_v/x/y/z pattern:
        // each is vfmacc.vf on ITS OWN destination register, no dependency
        // between them, mirroring the real kernel's inner-loop shape.
        asm volatile ("vfmacc.vf v3, %0, v2" :: "f"(1.5f));
        asm volatile ("vfmacc.vf v4, %0, v2" :: "f"(1.5f));
        asm volatile ("vfmacc.vf v5, %0, v2" :: "f"(1.5f));
        asm volatile ("vfmacc.vf v6, %0, v2" :: "f"(1.5f));
    }
    stop_timer();

    printf("[ROI-LATENCY]: %ld cycles\n", get_timer());
    printf("SUCCESS\n");
    return 0;
}
