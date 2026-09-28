// Independent-register VMFPU_FNONCOMP throughput probe (vfsgnjx.vv, used
// for abs() in blackscholes's CNDF chain), at vl=8/SEW32/LMUL1 -- matching
// blackscholes's real shape. FNONCOMP's occupancy has always been plain
// n_beats (never touched by the lookupRTL occupancy fix), so this checks
// whether it was being masked by the old FMA-lookup over-serialization
// (same class of bug, different category) and is now the real bottleneck.
#include <stdint.h>
#include "printf.h"
#include "runtime.h"

#define REPS 32
static float a[8] __attribute__((aligned(64)));

int main() {
    HW_CNT_READY;
    for (int i = 0; i < 8; i++) a[i] = (float)(i - 4);
    long gvl;
    asm volatile ("vsetvli %0, %1, e32, m1, ta, ma" : "=r"(gvl) : "r"(8));
    asm volatile ("vle32.v v1, (%0)" :: "r"(a));
    asm volatile ("vle32.v v2, (%0)" :: "r"(a));
    asm volatile ("vle32.v v3, (%0)" :: "r"(a));
    asm volatile ("vle32.v v4, (%0)" :: "r"(a));

    start_timer();
    for (int r = 0; r < REPS; r++) {
        // 4 independent destinations, no dependency between them.
        asm volatile ("vfsgnjx.vv v1, v1, v1");
        asm volatile ("vfsgnjx.vv v2, v2, v2");
        asm volatile ("vfsgnjx.vv v3, v3, v3");
        asm volatile ("vfsgnjx.vv v4, v4, v4");
    }
    stop_timer();

    printf("[ROI-LATENCY]: %ld cycles\n", get_timer());
    printf("SUCCESS\n");
    return 0;
}
