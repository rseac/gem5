// VL-sweep unit-stride LOAD probe (SEW=64, LMUL=1), independent registers --
// characterizes computeUnitLoad()'s fixed "14" dispatch constant
// (2*log2(NrClusters) + 14 + 2*n_beats, recalibrated 2026-09 via
// ubench_mem_hazards) across the full n_beats range at 4 lanes: VL=1/2/4
// all give n_beats=1, VL=8 gives n_beats=2, VL=16 gives n_beats=4, VL=32
// gives n_beats=8, VL=64 (=VLMAX at 4L/VLEN=4096) gives n_beats=16 -- the
// point ubench_mem_hazards most likely originally calibrated at, since
// that's the natural VLMAX a real application's vsetvl would produce.
// particlefilter's VLSU_UNIT_LD debug average (18.05 cyc/instr across
// 16420 instances) implies n_beats~2 dominates its real usage, and the
// VL=8 point alone already showed a ~182% overestimate (6.375 measured vs
// 18 predicted) -- this sweep checks whether the fixed cost is wrong
// uniformly or only breaks down below some VL threshold.
#include <stdint.h>
#include "printf.h"
#include "runtime.h"

#define REPS 32
static double buf[4 * 64] __attribute__((aligned(64)));

static void unit_load_chain(int64_t vl) {
    asm volatile("vsetvli zero, %0, e64, m1, ta, ma" : : "r"(vl));
    start_timer();
    for (int r = 0; r < REPS; r++) {
        // 4 independent destinations, no dependency between them.
        asm volatile("vle64.v v1, (%0)" :: "r"(buf));
        asm volatile("vle64.v v2, (%0)" :: "r"(buf + 64));
        asm volatile("vle64.v v3, (%0)" :: "r"(buf + 128));
        asm volatile("vle64.v v4, (%0)" :: "r"(buf + 192));
    }
    stop_timer();
    printf("[VL]: %ld [N]: %d [ROI-LATENCY]: %ld cycles\n", vl, REPS * 4, get_timer());
}

int main() {
    HW_CNT_READY;
    for (int i = 0; i < 4 * 64; i++) buf[i] = (double)i;

    static const int64_t vls[] = {1, 2, 4, 8, 16, 32, 64};
    for (unsigned i = 0; i < sizeof(vls) / sizeof(vls[0]); i++) {
        unit_load_chain(vls[i]);
    }
    printf("SUCCESS\n");
    return 0;
}
