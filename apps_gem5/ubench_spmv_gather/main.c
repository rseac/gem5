// Faithful reproduction of spmv's real indexed-load (gather) usage
// (RiVEC _spmv/src/spmv.c, spmv_intrinsics): per row, load column indices
// (unit-stride, e64), shift left by 3 to get byte offsets, then an
// indexed/gather load of x[] using those offsets, then a MACC accumulate.
// computeGather()'s current formula (LOW confidence, per its own comment
// in ara_timing.cpp) uses a fixed floor (57-67 cycles depending on lanes)
// plus a small per-element term -- calibrated at some point, but never
// checked against spmv's REAL small-vl (sparse row width) usage. This
// probe sweeps vl to characterize the real relationship.
#include <stdint.h>
#include "printf.h"
#include "runtime.h"

#define NROWS 64
static uint64_t ja[16] __attribute__((aligned(64)));
static double x[256] __attribute__((aligned(64)));
static double a[16] __attribute__((aligned(64)));

static void gather_row(int64_t vl) {
    asm volatile("vsetvli zero, %0, e64, m1, ta, ma" : : "r"(vl));
    asm volatile("vle64.v v1, (%0)" :: "r"(ja));      // column indices
    asm volatile("vle64.v v2, (%0)" :: "r"(a));        // matrix values
    start_timer();
    for (int r = 0; r < NROWS; r++) {
        asm volatile("vsll.vi v3, v1, 3");             // idx -> byte offset
        asm volatile("vluxei64.v v4, (%0), v3" :: "r"(x)); // gather x[]
        asm volatile("vfmacc.vv v5, v2, v4");          // accumulate
    }
    stop_timer();
    printf("[VL]: %ld [ROI-LATENCY]: %ld cycles\n", vl, get_timer());
}

int main() {
    HW_CNT_READY;
    for (int i = 0; i < 16; i++) { ja[i] = (uint64_t)(i % 8); a[i] = 1.0 + i; }
    for (int i = 0; i < 256; i++) x[i] = (double)i;

    static const int64_t vls[] = {2, 4, 8, 16};
    for (unsigned i = 0; i < sizeof(vls) / sizeof(vls[0]); i++) {
        gather_row(vls[i]);
    }
    printf("SUCCESS\n");
    return 0;
}
