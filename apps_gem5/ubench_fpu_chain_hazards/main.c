// FMA-Chain ("mul -> madd -> sub") Calibration Microbenchmark (RTL)
//
// Targets VP++'s lookupRTL() exact-match calibration table for VMFPU_FMA
// (ara_timing.cpp: rtl_fpu_ew32_vl16_/vl256_/vl1024_ and the ew64 variants),
// which is currently a hand-derived, hardcoded set of per-(lane,VLEN,vl-
// bucket) constants (see the AraTimingModel constructor) rather than data
// fit from a clean sweep. Root-caused earlier as the dominant driver of
// blackscholes's ~2x ROI overestimate at >=4 lanes (its CNDF/exp/log
// polynomial is almost entirely SEW32, LMUL1, vl<=16 vfadd/vfsub/vfmul/
// vfmadd/vfmacc chains, all hitting this exact lookup path).
//
// Chain: vfmul.vv -> vfmacc.vv -> vfsub.vv, each step depending on the
// previous (v8 accumulates), repeated 10x = 30 dependent FMA-classified
// instructions per run - a realistic stand-in for blackscholes/CNDF-style
// polynomial evaluation, not just a single repeated op like ubench_hazards.
//
// Run at both e32 and e64 (LMUL=1 in both cases, matching lookupRTL's exact
// match condition lmul_num==1 && lmul_den==1), sweeping vl, so
// T(vl) = a + b*vl can be fit per (lane, VLEN, sew) the same way the memory
// ubenches were fit - replacing the hardcoded 3-bucket lookup table with
// real, continuous calibration data.
#include <stdint.h>
#include "printf.h"
#include "runtime.h"

static void fma_chain_ew32(int64_t vl) {
    asm volatile("vsetvli zero, %0, e32, m1, ta, ma" : : "r"(vl));
    start_timer();
    for (int i = 0; i < 10; i++) {
        asm volatile("vfmul.vv  v8, v8, v9");
        asm volatile("vfmacc.vv v8, v10, v11");
        asm volatile("vfsub.vv  v8, v8, v12");
    }
    stop_timer();
    printf("[VL]: %ld [SEW]: 32 [OP]: fma_chain [ROI-LATENCY]: %ld cycles\n", vl, get_timer());
}

static void fma_chain_ew64(int64_t vl) {
    asm volatile("vsetvli zero, %0, e64, m1, ta, ma" : : "r"(vl));
    start_timer();
    for (int i = 0; i < 10; i++) {
        asm volatile("vfmul.vv  v8, v8, v9");
        asm volatile("vfmacc.vv v8, v10, v11");
        asm volatile("vfsub.vv  v8, v8, v12");
    }
    stop_timer();
    printf("[VL]: %ld [SEW]: 64 [OP]: fma_chain [ROI-LATENCY]: %ld cycles\n", vl, get_timer());
}

int main() {
    HW_CNT_READY;
    printf("=== ubench_fpu_chain_hazards (NR_LANES=%d VLEN=%d) ===\n", NR_LANES, VLEN);
    static const int64_t vls[] = {16, 32, 64, 128, 256, 512, 1024};
    for (unsigned i = 0; i < sizeof(vls) / sizeof(vls[0]); i++) {
        fma_chain_ew32(vls[i]);
        fma_chain_ew64(vls[i]);
    }
    printf("SUCCESS\n");
    return 0;
}
