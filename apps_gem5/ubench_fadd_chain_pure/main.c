// Pure simple-op (non-fused) FP chain calibration: vfmul.vv x4, single
// dependent chain, matching blackscholes's REAL xK2_2->xK2_3->xK2_4->xK2_5
// usage exactly (CNDF_SIMD, RiVEC _blackscholes). Used to calibrate the
// NEW VMFPU_FADD category (split from VMFPU_FMA, which conflated simple
// vfadd/vfsub/vfmul with fused vfmacc/vfmadd -- the same conflation
// AraXL's fix #9 already resolved, never ported to Ara) with real,
// uncontaminated single-op-type data, rather than reusing the old
// lookup table's values (calibrated from a MIXED mul->madd->sub chain).
#include <stdint.h>
#include "printf.h"
#include "runtime.h"

static void fadd_chain(int64_t vl, int sew) {
    if (sew == 32)
        asm volatile("vsetvli zero, %0, e32, m1, ta, ma" : : "r"(vl));
    else
        asm volatile("vsetvli zero, %0, e64, m1, ta, ma" : : "r"(vl));
    start_timer();
    for (int i = 0; i < 10; i++) {
        asm volatile("vfmul.vv v8, v8, v9");
        asm volatile("vfmul.vv v8, v8, v9");
        asm volatile("vfmul.vv v8, v8, v9");
        asm volatile("vfmul.vv v8, v8, v9");
    }
    stop_timer();
    printf("[VL]: %ld [SEW]: %d [OP]: fadd_chain [ROI-LATENCY]: %ld cycles\n", vl, sew, get_timer());
}

int main() {
    HW_CNT_READY;
    printf("=== ubench_fadd_chain_pure (NR_LANES=%d VLEN=%d) ===\n", NR_LANES, VLEN);
    static const int64_t vls[] = {8, 16, 128, 256, 512, 1024};
    for (unsigned i = 0; i < sizeof(vls) / sizeof(vls[0]); i++) {
        fadd_chain(vls[i], 32);
        fadd_chain(vls[i], 64);
    }
    printf("SUCCESS\n");
    return 0;
}
