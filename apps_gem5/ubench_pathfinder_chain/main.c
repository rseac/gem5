// Calibration probe: reproduces pathfinder's exact real dependency chain
// (confirmed via riscv-vectorized-benchmark-suite/_pathfinder/src/main.cpp:
// vle32.v -> vslide1up.vx -> vmin.vv -> vslide1down.vx -> vmin.vv ->
// vle32.v -> vadd.vv -> vse32.v), at the same LMUL=8/SEW=32 the real
// benchmark uses, to isolate whether Ara's scoreboard under-charges this
// dependency chain. Suspected mechanism: l_fe (the "first element ready"
// latency used for RAW-hazard chaining) is hardcoded to 1 cycle for every
// non-FPU category (VALU, VSLIDE, VLSU included) in v.h -- letting a
// dependent instruction start reading its source just 1 cycle after the
// PRODUCER's issue, not its completion. Real memory (VLSU) and slide
// (VSLIDE) almost certainly cannot forward results that fast.
#include <stdint.h>
#include "printf.h"
#include "runtime.h"

#define COLS 512
#define ROWS 64
static int32_t wall[ROWS * COLS] __attribute__((aligned(64)));
static int32_t dst[COLS] __attribute__((aligned(64)));

int main() {
    HW_CNT_READY;
    for (int i = 0; i < ROWS * COLS; i++) wall[i] = i % 100;

    start_timer();
    for (int t = 0; t < ROWS - 1; t++) {
        int32_t aux = 0x7fffffff, aux2 = 0x7fffffff;
        long gvl;
        for (int n = 0; n < COLS; n += gvl) {
            asm volatile ("vsetvli %0, %1, e32, m8, ta, ma" : "=r"(gvl) : "r"(COLS - n));
            asm volatile ("vle32.v v0, (%0)" :: "r"(&dst[n]));
            asm volatile ("vle32.v v24, (%0)" :: "r"(&wall[(t + 1) * COLS + n]));
            asm volatile ("vslide1up.vx v16, v0, %0" :: "r"(aux));
            asm volatile ("vmin.vv v0, v0, v16");
            asm volatile ("vslide1down.vx v8, v0, %0" :: "r"(aux2));
            asm volatile ("vmin.vv v0, v0, v8");
            asm volatile ("vadd.vv v0, v0, v24");
            asm volatile ("vse32.v v0, (%0)" :: "r"(&dst[n]));
        }
    }
    stop_timer();

    printf("[ROI-LATENCY]: %ld cycles\n", get_timer());
    printf("SUCCESS\n");
    return 0;
}
