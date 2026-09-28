// Calibration probe v2: isolates vcpop/vfirst/viota's real cost cleanly.
// v1 (load->compare->cpop->first->iota per iteration) failed to isolate
// anything: the surrounding loads (each ~32 cyc real cost) provided far
// more natural spacing than any latency assigned to the scan ops, so
// changing their formula never moved the observed ROI at all -- confirmed
// empirically (507 cycles before AND after fixing computeMask()). This
// version loads ONCE outside the timed region, then repeats the
// compare->cpop->first->iota chain many times with NO memory access in
// the loop, so the scan ops' own cost is what actually gates the timing.
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

    long cpop_result = 0, first_result = 0;
    start_timer();
    for (int r = 0; r < REPS; r++) {
        asm volatile ("vmflt.vv v0, v1, v2");     // compare -> mask v0
        asm volatile ("vcpop.m %0, v0" : "=r"(cpop_result));
        asm volatile ("vfirst.m %0, v0" : "=r"(first_result));
        asm volatile ("viota.m v3, v0");
    }
    stop_timer();
    if (cpop_result == -1 && first_result == -1) printf("unreachable\n");

    printf("[ROI-LATENCY]: %ld cycles\n", get_timer());
    printf("SUCCESS\n");
    return 0;
}
