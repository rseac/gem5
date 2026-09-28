// Isolated strided-STORE-only probe (SEW=64, LMUL=1) -- see
// ubench_stride64_load_only/main.c for the full rationale. Measures vsse64
// ALONE (4 independent sources, 4 independent destination buffers, no
// dependency between them) at the same req_vl=512 (>256, vl1024_ bucket)
// so the true store-only cost can be compared directly against the
// load-only probe and against the existing combined pair_cost.
#include <stdint.h>
#include "printf.h"
#include "runtime.h"

#define REPS 16
static double buf[4 * 512] __attribute__((aligned(64)));

static void strided_store_chain(int64_t vl) {
    asm volatile("vsetvli zero, %0, e64, m1, ta, ma" : : "r"(vl));
    // Pre-load source registers once, outside the timed region, so the
    // timed loop is pure store issue/dispatch cost with no load dependency.
    asm volatile("vlse64.v v1, (%0), %1" :: "r"(buf), "r"(32));
    asm volatile("vlse64.v v2, (%0), %1" :: "r"(buf + 1), "r"(32));
    asm volatile("vlse64.v v3, (%0), %1" :: "r"(buf + 2), "r"(32));
    asm volatile("vlse64.v v4, (%0), %1" :: "r"(buf + 3), "r"(32));

    start_timer();
    for (int r = 0; r < REPS; r++) {
        // 4 independent destinations (different base pointers), no
        // dependency between them and no dependency on each other's
        // completion.
        asm volatile("vsse64.v v1, (%0), %1" :: "r"(buf), "r"(32));
        asm volatile("vsse64.v v2, (%0), %1" :: "r"(buf + 1), "r"(32));
        asm volatile("vsse64.v v3, (%0), %1" :: "r"(buf + 2), "r"(32));
        asm volatile("vsse64.v v4, (%0), %1" :: "r"(buf + 3), "r"(32));
    }
    stop_timer();
    printf("[VL]: %ld [N]: %d [ROI-LATENCY]: %ld cycles\n", vl, REPS * 4, get_timer());
}

int main() {
    HW_CNT_READY;
    for (int i = 0; i < 4 * 512; i++) buf[i] = (double)i;

    strided_store_chain(512);
    printf("SUCCESS\n");
    return 0;
}
