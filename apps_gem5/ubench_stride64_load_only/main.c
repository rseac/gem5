// Isolated strided-LOAD-only probe (SEW=64, LMUL=1), separating vlse64 from
// vsse64 to challenge ara_timing.cpp's lookupRTL() 50/50 pair-cost split
// (rtl_vlsu_stride_vl16_/vl256_/vl1024_): the RTL calibration data behind
// that split was measured as a combined vlse+vsse PAIR and then attributed
// ceil(pair/2) to load / floor(pair/2) to store -- an assumption, never
// independently validated. computeUnitStore()'s own prior recalibration
// found unit-stride stores have near-zero fixed dispatch cost vs. loads
// (no destination register/response to wait on) -- the same physical
// reason should apply to strided stores too, meaning the current 50/50
// split likely over-attributes cost to the load side. This probe measures
// vlse64 ALONE (4 independent destinations, no dependency between them,
// matching imatmul's real requested VL > 256 that lands in the vl1024_
// lookup bucket) so the true load-only cost can be compared directly
// against ubench_stride64_store_only's store-only cost and against the
// existing combined pair_cost (249 @ 4L/VLEN=4096/req_vl>256).
#include <stdint.h>
#include "printf.h"
#include "runtime.h"

#define REPS 16
static double buf[4 * 512] __attribute__((aligned(64)));  // 4 interleaved fields, stride=32B (4*8B elems)

static void strided_load_chain(int64_t vl) {
    asm volatile("vsetvli zero, %0, e64, m1, ta, ma" : : "r"(vl));
    start_timer();
    for (int r = 0; r < REPS; r++) {
        // 4 independent destinations, no dependency between them.
        asm volatile("vlse64.v v1, (%0), %1" :: "r"(buf), "r"(32));
        asm volatile("vlse64.v v2, (%0), %1" :: "r"(buf + 1), "r"(32));
        asm volatile("vlse64.v v3, (%0), %1" :: "r"(buf + 2), "r"(32));
        asm volatile("vlse64.v v4, (%0), %1" :: "r"(buf + 3), "r"(32));
    }
    stop_timer();
    printf("[VL]: %ld [N]: %d [ROI-LATENCY]: %ld cycles\n", vl, REPS * 4, get_timer());
}

int main() {
    HW_CNT_READY;
    for (int i = 0; i < 4 * 512; i++) buf[i] = (double)i;

    // req_vl=512 (>256) deliberately lands in the same "vl1024_" lookup
    // bucket imatmul's real strided loads hit -- the hardware clamps to
    // VLMAX internally (matches lookupRTL()'s own "use raw VL request,
    // not effective VL" indexing).
    strided_load_chain(512);
    printf("SUCCESS\n");
    return 0;
}
