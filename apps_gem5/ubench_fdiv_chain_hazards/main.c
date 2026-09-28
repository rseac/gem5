// FP Divide/Sqrt Dependency-Chain Calibration Microbenchmark (RTL)
//
// Targets computeIDIV(), which VMFPU_FDIV (real vfdiv.vv/vfsqrt.v - see
// ara_timing_classify.h) is routed through as a "serial divider
// approximation" (ara_timing.cpp comment), using the same cost formula as
// genuine serial integer division: n_beats * elems_per_beat * (sew+2).
// Root-caused as the dominant cost driver for somier (avg 273 cycles/instr,
// 63% of its whole predicted ROI) and swaptions (avg 537 cycles/instr) -
// both do real physics/finance FP division and sqrt (somier:
// dl=sqrt(dx^2+dy^2+dz^2), force=dx/dl; swaptions: CumNormalInv, HJM vol
// calcs), not integer division.
//
// Chain: vfdiv.vv (dependent, v8 accumulates) and vfsqrt.v, each 10x = 10
// dependent instructions per run (kept shorter than the FMA chain since
// real division/sqrt units are commonly NOT fully pipelined - a long
// dependent chain would need bigger buffers to avoid saturating other
// resources first). Swept over vl and sew like the FMA-chain ubench, so
// T(vl) = a + b*vl can replace computeIDIV's assumption for this FU.
#include <stdint.h>
#include "printf.h"
#include "runtime.h"

static void fdiv_chain_ew32(int64_t vl) {
    asm volatile("vsetvli zero, %0, e32, m1, ta, ma" : : "r"(vl));
    start_timer();
    for (int i = 0; i < 10; i++) {
        asm volatile("vfdiv.vv v8, v8, v9");
    }
    stop_timer();
    printf("[VL]: %ld [SEW]: 32 [OP]: fdiv_chain [ROI-LATENCY]: %ld cycles\n", vl, get_timer());
}

static void fdiv_chain_ew64(int64_t vl) {
    asm volatile("vsetvli zero, %0, e64, m1, ta, ma" : : "r"(vl));
    start_timer();
    for (int i = 0; i < 10; i++) {
        asm volatile("vfdiv.vv v8, v8, v9");
    }
    stop_timer();
    printf("[VL]: %ld [SEW]: 64 [OP]: fdiv_chain [ROI-LATENCY]: %ld cycles\n", vl, get_timer());
}

static void fsqrt_chain_ew32(int64_t vl) {
    asm volatile("vsetvli zero, %0, e32, m1, ta, ma" : : "r"(vl));
    start_timer();
    for (int i = 0; i < 10; i++) {
        asm volatile("vfsqrt.v v8, v8");
    }
    stop_timer();
    printf("[VL]: %ld [SEW]: 32 [OP]: fsqrt_chain [ROI-LATENCY]: %ld cycles\n", vl, get_timer());
}

static void fsqrt_chain_ew64(int64_t vl) {
    asm volatile("vsetvli zero, %0, e64, m1, ta, ma" : : "r"(vl));
    start_timer();
    for (int i = 0; i < 10; i++) {
        asm volatile("vfsqrt.v v8, v8");
    }
    stop_timer();
    printf("[VL]: %ld [SEW]: 64 [OP]: fsqrt_chain [ROI-LATENCY]: %ld cycles\n", vl, get_timer());
}

int main() {
    HW_CNT_READY;
    printf("=== ubench_fdiv_chain_hazards (NR_LANES=%d VLEN=%d) ===\n", NR_LANES, VLEN);
    static const int64_t vls[] = {16, 32, 64, 128, 256, 512, 1024};
    for (unsigned i = 0; i < sizeof(vls) / sizeof(vls[0]); i++) {
        fdiv_chain_ew32(vls[i]);
        fdiv_chain_ew64(vls[i]);
        fsqrt_chain_ew32(vls[i]);
        fsqrt_chain_ew64(vls[i]);
    }
    printf("SUCCESS\n");
    return 0;
}
