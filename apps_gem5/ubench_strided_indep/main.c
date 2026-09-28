// Independent-register strided-load occupancy probe, matching lavamd's
// REAL usage exactly (RiVEC _lavaMD kernel_vec: _MM_LOAD_STRIDE_f32(&rB[j].v,
// 16, gvl), e32/m1, stride=16 bytes, vl up to 32). computeStridedLoad()'s
// own latency formula is just computeUnitLoad()+a small crossbar term
// (MEDIUM confidence -- unlike gather's LOW confidence), so its true
// structural occupancy may resemble unit-stride's, not gather's fully-
// serial treatment it inherited by sharing fu_idx==2. This probe measures
// real back-to-back INDEPENDENT strided-load throughput directly, rather
// than assuming by analogy.
#include <stdint.h>
#include "printf.h"
#include "runtime.h"

#define REPS 32
static float buf[4 * 32] __attribute__((aligned(64)));  // 4 interleaved fields, 32 elems each, stride=16B

static void strided_chain(int64_t vl) {
    asm volatile("vsetvli zero, %0, e32, m1, ta, ma" : : "r"(vl));
    start_timer();
    for (int r = 0; r < REPS; r++) {
        // 4 independent destinations, no dependency between them --
        // matches lavamd loading rB[j].v/.x/.y/.z (stride=16B) into 4
        // separate accumulator-feed registers per inner-loop iteration.
        asm volatile("vlse32.v v1, (%0), %1" :: "r"(buf), "r"(16));
        asm volatile("vlse32.v v2, (%0), %1" :: "r"(buf + 1), "r"(16));
        asm volatile("vlse32.v v3, (%0), %1" :: "r"(buf + 2), "r"(16));
        asm volatile("vlse32.v v4, (%0), %1" :: "r"(buf + 3), "r"(16));
    }
    stop_timer();
    printf("[VL]: %ld [ROI-LATENCY]: %ld cycles\n", vl, get_timer());
}

int main() {
    HW_CNT_READY;
    for (int i = 0; i < 4 * 32; i++) buf[i] = (float)i;

    static const int64_t vls[] = {8, 16, 32};
    for (unsigned i = 0; i < sizeof(vls) / sizeof(vls[0]); i++) {
        strided_chain(vls[i]);
    }
    printf("SUCCESS\n");
    return 0;
}
