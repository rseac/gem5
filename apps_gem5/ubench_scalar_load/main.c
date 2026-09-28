// Isolated SCALAR CVA6 load-latency probe -- no vector instructions at
// all. Challenges scalar_latencies.md's "Load (L1 D-cache hit): 3 cycles,
// High confidence" entry -- "High confidence" here means grounded in a
// structural RTL citation (CVA6ConfigNrLoadPipeRegs=1, 3-cycle load-use
// path), NOT empirical measurement: the report's own Validation Status
// says "Empirical validation: NOT performed" for the entire scalar table,
// this entry included. FLD (scalar FP64 load) was found to be the single
// largest scalar cost driver, by dynamic instruction volume, across all
// three of Ara's current worst-MAPE benchmarks (particlefilter, imatmul,
// swaptions) -- making it the highest-leverage untested constant left.
//
// Each asm block's output is a plain register (reused each iteration, no
// per-iteration store-back to memory) so the ONLY thing in the loop body
// besides the load itself is the induction variable's own overhead --
// isolated separately by ubench_scalar_loop_overhead, same loop shape,
// same "reused register" pattern, load replaced by a trivial ALU op.
#include <stdint.h>
#include "printf.h"
#include "runtime.h"

#define REPS 20

static double dbuf[REPS] __attribute__((aligned(64)));
static int32_t wbuf[REPS] __attribute__((aligned(64)));
static int64_t lbuf[REPS] __attribute__((aligned(64)));

int main() {
    HW_CNT_READY;
    for (int i = 0; i < REPS; i++) {
        dbuf[i] = (double)i + 0.5;
        wbuf[i] = i * 7;
        lbuf[i] = (int64_t)i * 1000000007LL;
    }

    double dsink;
    int32_t wsink;
    int64_t lsink;

    start_timer();
    for (int i = 0; i < REPS; i++) {
        asm volatile("fld %0, %1" : "=f"(dsink) : "m"(dbuf[i]));
    }
    stop_timer();
    printf("[OP]: fld [N]: %d [ROI-LATENCY]: %ld cycles\n", REPS, get_timer());

    start_timer();
    for (int i = 0; i < REPS; i++) {
        asm volatile("lw %0, %1" : "=r"(wsink) : "m"(wbuf[i]));
    }
    stop_timer();
    printf("[OP]: lw [N]: %d [ROI-LATENCY]: %ld cycles\n", REPS, get_timer());

    start_timer();
    for (int i = 0; i < REPS; i++) {
        asm volatile("ld %0, %1" : "=r"(lsink) : "m"(lbuf[i]));
    }
    stop_timer();
    printf("[OP]: ld [N]: %d [ROI-LATENCY]: %ld cycles\n", REPS, get_timer());

    printf("SUCCESS: %f %d %ld\n", dsink, wsink, lsink);
    return 0;
}
