// Pure fused-op FP chain calibration: vfmacc.vv x4, single dependent
// chain (no simple vfmul/vfadd/vfsub mixed in), matching blackscholes's
// xLocal_2 accumulation pattern and lavamd's xfA_v-style accumulation.
// Used to (re)calibrate VMFPU_FMA once it is split to mean ONLY fused
// 3-operand ops (vfmacc/vfmadd/etc.), since the old lookup table's "9
// cycles" value was derived from a chain that was 2/3 simple ops.
#include <stdint.h>
#include "printf.h"
#include "runtime.h"

static void fma_chain(int64_t vl, int sew) {
    if (sew == 32)
        asm volatile("vsetvli zero, %0, e32, m1, ta, ma" : : "r"(vl));
    else
        asm volatile("vsetvli zero, %0, e64, m1, ta, ma" : : "r"(vl));
    start_timer();
    for (int i = 0; i < 10; i++) {
        asm volatile("vfmacc.vv v8, v10, v11");
        asm volatile("vfmacc.vv v8, v10, v11");
        asm volatile("vfmacc.vv v8, v10, v11");
        asm volatile("vfmacc.vv v8, v10, v11");
    }
    stop_timer();
    printf("[VL]: %ld [SEW]: %d [OP]: fma_chain [ROI-LATENCY]: %ld cycles\n", vl, sew, get_timer());
}

int main() {
    HW_CNT_READY;
    printf("=== ubench_fma_chain_pure (NR_LANES=%d VLEN=%d) ===\n", NR_LANES, VLEN);
    static const int64_t vls[] = {8, 16, 128, 256, 512, 1024};
    for (unsigned i = 0; i < sizeof(vls) / sizeof(vls[0]); i++) {
        fma_chain(vls[i], 32);
        fma_chain(vls[i], 64);
    }
    printf("SUCCESS\n");
    return 0;
}
