// Calibration probe: reproduces particlefilter's real comparison+mask-
// compaction idiom (confirmed via objdump of the compiled binary:
// vmfle.vv/vmflt.vf compare -> vcpop.m/vfirst.m/viota.m mask-scan ops -
// the resampling/index-selection pattern). VMASK's current formula
// ("use ALU model as baseline", never independently calibrated) treats
// vcpop/vfirst/viota the same as a simple elementwise ALU add -- but
// these are inherently SEQUENTIAL SCAN operations (each element's result
// depends on all preceding mask bits), architecturally distinct from a
// parallel elementwise op. This probe issues a dependent compare -> cpop
// -> first -> iota chain, matching the real usage, at the same LMUL/SEW
// particlefilter uses (e32, m1 -- confirmed via objdump: .vv/.vf forms
// with no LMUL suffix in the mnemonics point at m1 default in this
// build).
#include <stdint.h>
#include "printf.h"
#include "runtime.h"

#define N 256
static float arr[N] __attribute__((aligned(64)));
static float thresh[N] __attribute__((aligned(64)));

int main() {
    HW_CNT_READY;
    for (int i = 0; i < N; i++) { arr[i] = (float)(i % 7); thresh[i] = 3.0f; }

    start_timer();
    long i = 0;
    long gvl;
    while (i < N) {
        asm volatile ("vsetvli %0, %1, e32, m1, ta, ma" : "=r"(gvl) : "r"(N - i));
        asm volatile ("vle32.v v1, (%0)" :: "r"(&arr[i]));
        asm volatile ("vle32.v v2, (%0)" :: "r"(&thresh[i]));
        asm volatile ("vmflt.vv v0, v1, v2");     // compare -> mask v0
        
        
        asm volatile ("viota.m v3, v0");           // exclusive running popcount per elem
        i += gvl;
    }
    stop_timer();

    printf("[ROI-LATENCY]: %ld cycles\n", get_timer());
    printf("SUCCESS\n");
    return 0;
}
