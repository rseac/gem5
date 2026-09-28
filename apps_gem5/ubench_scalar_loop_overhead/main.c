// Loop-overhead baseline for ubench_scalar_load: SAME loop shape (for
// loop, REPS=20, asm volatile with a plain reused-register output) but
// with a trivial register-only ALU op instead of a memory load, to
// isolate how much of that probe's measured cycles/iteration is genuine
// load latency vs. loop-control overhead (induction variable
// increment/compare/branch) the compiler emits around the asm block.
#include <stdint.h>
#include "printf.h"
#include "runtime.h"

#define REPS 20

int main() {
    HW_CNT_READY;
    int64_t sink;

    start_timer();
    for (int i = 0; i < REPS; i++) {
        asm volatile("addi %0, x0, %1" : "=r"(sink) : "i"(1));
    }
    stop_timer();
    printf("[OP]: addi_loop [N]: %d [ROI-LATENCY]: %ld cycles\n", REPS, get_timer());

    printf("SUCCESS: %ld\n", sink);
    return 0;
}
