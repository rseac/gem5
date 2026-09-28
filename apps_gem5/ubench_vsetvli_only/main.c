// vsetvli-only Isolation Microbenchmark (RTL)
//
// Tests whether VP++'s `vsetvli_cost` constant (ara_timing.cpp, defined as
// `(nl >= 4) ? 11 : 7`, a function of lane count ONLY, with no VLEN term)
// matches real RTL. Motivated by the blackscholes MAPE investigation: its
// compiled binary contains 10 vsetvl instructions, and VP++'s predicted
// cycle count is completely flat across VLEN (1024/2048/4096) at a fixed
// lane count, while RTL shows real growth with VLEN. This probe isolates
// vsetvli cost alone -- no vector compute between calls -- alternating
// SEW32/SEW64 each iteration to force genuine reconfiguration (avoiding any
// "same vtype, skip" fast path that would hide the real cost).
//
// Run identically (same instructions) across all 9 (lane, VLEN) RTL builds.
// If real RTL cycle count for this probe grows with VLEN at a fixed lane
// count, the hypothesis is confirmed: vsetvli_cost is missing a VLEN term.
// If it's flat like VP++ predicts, the hypothesis is wrong and the residual
// blackscholes gap is coming from somewhere else.
#include <stdint.h>
#include "printf.h"
#include "runtime.h"

int main() {
    HW_CNT_READY;
    printf("=== ubench_vsetvli_only (NR_LANES=%d VLEN=%d) ===\n", NR_LANES, VLEN);

    start_timer();
    for (int i = 0; i < 10; i++) {
        asm volatile("vsetvli zero, %0, e32, m1, ta, ma" : : "r"(8));
        asm volatile("vsetvli zero, %0, e64, m1, ta, ma" : : "r"(8));
    }
    stop_timer();
    printf("[OP]: vsetvli_only [N]: 20 [ROI-LATENCY]: %ld cycles\n", get_timer());

    printf("SUCCESS\n");
    return 0;
}
