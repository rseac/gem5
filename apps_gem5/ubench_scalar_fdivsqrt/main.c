// Isolated SCALAR CVA6 fdiv.d/fsqrt.d probe -- no vector instructions at
// all. Challenges scalar_latencies.md's "FP Div/Sqrt (FP64): FDIV_D,
// FSQRT_D -> 20 (average model)" entry, explicitly flagged "Medium
// confidence... Empirical validation: NOT performed" in that same report.
// particlefilter (sqrt(pow(...)), weights[x]/sumWeights) and somier
// (dl=sqrt(dx*dx+dy*dy+dz*dz); FX=dx/dl; ...) both hammer exactly this
// instruction pair in their hottest scalar loops, and both are ~80%+
// scalar-dominated (confirmed via VPPP_TIMING_DEBUG_SPLIT) and both show
// large VP++ overestimates -- this probe checks whether real CVA6 fdiv.d/
// fsqrt.d latency actually matches the unvalidated 20-cycle guess.
// CVA6 is single-issue in-order with a non-pipelined iterative div/sqrt
// unit (serdiv.sv-class hardware), so back-to-back independent divides
// still serialize on the shared FU -- no special dependency-chain pattern
// needed to get the real per-instruction latency.
#include <stdint.h>
#include "printf.h"
#include "runtime.h"

#define REPS 20

int main() {
    HW_CNT_READY;
    static double a[REPS], b[REPS], c[REPS];
    for (int i = 0; i < REPS; i++) {
        a[i] = 100.0 + (double)i;
        b[i] = 3.0 + (double)i * 0.1;
    }

    start_timer();
    for (int i = 0; i < REPS; i++) {
        asm volatile("fdiv.d %0, %1, %2" : "=f"(c[i]) : "f"(a[i]), "f"(b[i]));
    }
    stop_timer();
    printf("[OP]: fdiv.d [N]: %d [ROI-LATENCY]: %ld cycles\n", REPS, get_timer());

    start_timer();
    for (int i = 0; i < REPS; i++) {
        asm volatile("fsqrt.d %0, %1" : "=f"(c[i]) : "f"(a[i]));
    }
    stop_timer();
    printf("[OP]: fsqrt.d [N]: %d [ROI-LATENCY]: %ld cycles\n", REPS, get_timer());

    printf("SUCCESS\n");
    return 0;
}
