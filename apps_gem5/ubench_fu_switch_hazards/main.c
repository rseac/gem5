// FU-Switch Penalty + FNONCOMP/FCONV Calibration Microbenchmark (RTL)
//
// Diagnoses blackscholes's residual ROI underestimate (VP++ now predicts
// LOWER than RTL after the FMA/FDIV/FSQRT fixes - the opposite direction
// from before). Two candidate causes tested here:
//
// 1. VMFPU_FNONCOMP (vfsgnjx, vfmin, vfmax, vmerge) and VMFPU_FCONV
//    (vfcvt) were never RTL-calibrated this session - their original
//    analytical formulas (log2(NC)+8+n_beats, log2(NC)+9+n_beats) might
//    simply be wrong, independent of anything else.
// 2. blackscholes's real code interleaves many different FU types very
//    tightly (FMA -> FNONCOMP -> FCONV -> ALU -> FMA -> FDIV ...). Every
//    calibration chain so far (FMA, FDIV, FSQRT) used a single repeated
//    instruction type - none of them could reveal a real hardware cost for
//    SWITCHING between FU types (pipeline drain/refill), which the model
//    currently charges nothing for.
//
// same_fu_chain: 30 dependent vfsgnjx.vv (FNONCOMP) - isolated baseline.
// same_fu_fcvt_chain: 30 dependent vfcvt.x.f.v / vfcvt.f.x.v pairs alternating
//   (FCONV only, still same FU class) - isolated FCONV baseline.
// alternating_chain: FMA -> FNONCOMP -> FCONV cycled 10x = 30 instructions,
//   same total instruction count/types as blackscholes's dominant mix, but
//   arranged to maximize FU-switch frequency. Compare its per-instruction
//   cost against the isolated FMA (9, already known) and FNONCOMP/FCONV
//   baselines measured here - any extra cost is the FU-switch penalty.
#include <stdint.h>
#include "printf.h"
#include "runtime.h"

static void fnoncomp_chain_ew32(int64_t vl) {
    asm volatile("vsetvli zero, %0, e32, m1, ta, ma" : : "r"(vl));
    start_timer();
    for (int i = 0; i < 30; i++) {
        asm volatile("vfsgnjx.vv v8, v8, v8");
    }
    stop_timer();
    printf("[VL]: %ld [OP]: fnoncomp_chain [ROI-LATENCY]: %ld cycles\n", vl, get_timer());
}

static void fconv_chain_ew32(int64_t vl) {
    asm volatile("vsetvli zero, %0, e32, m1, ta, ma" : : "r"(vl));
    start_timer();
    for (int i = 0; i < 15; i++) {
        asm volatile("vfcvt.x.f.v v8, v8");
        asm volatile("vfcvt.f.x.v v8, v8");
    }
    stop_timer();
    printf("[VL]: %ld [OP]: fconv_chain [ROI-LATENCY]: %ld cycles\n", vl, get_timer());
}

static void alternating_chain_ew32(int64_t vl) {
    asm volatile("vsetvli zero, %0, e32, m1, ta, ma" : : "r"(vl));
    start_timer();
    for (int i = 0; i < 10; i++) {
        asm volatile("vfmul.vv   v8, v8, v9");   // VMFPU_FMA
        asm volatile("vfsgnjx.vv v8, v8, v8");   // VMFPU_FNONCOMP
        asm volatile("vfcvt.x.f.v v10, v8");     // VMFPU_FCONV (separate dest to avoid corrupting the fp chain)
        asm volatile("vfcvt.f.x.v v8, v10");     // VMFPU_FCONV back, keeps v8 a valid float for next vfmul
    }
    stop_timer();
    printf("[VL]: %ld [OP]: alternating_chain [ROI-LATENCY]: %ld cycles\n", vl, get_timer());
}

int main() {
    HW_CNT_READY;
    printf("=== ubench_fu_switch_hazards (NR_LANES=%d VLEN=%d) ===\n", NR_LANES, VLEN);
    static const int64_t vls[] = {8, 16, 32, 64};
    for (unsigned i = 0; i < sizeof(vls) / sizeof(vls[0]); i++) {
        fnoncomp_chain_ew32(vls[i]);
        fconv_chain_ew32(vls[i]);
        alternating_chain_ew32(vls[i]);
    }
    printf("SUCCESS\n");
    return 0;
}
