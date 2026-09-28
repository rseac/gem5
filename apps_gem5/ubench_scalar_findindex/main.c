// Faithful reproduction of particlefilter's real scalar hot loop:
// findIndex() (RiVEC _particlefilter/src/main.c, lines 332-354) -- a
// linear scan over a double array (the CDF, length Nparticles=256) with
// a data-dependent early-exit branch, called once per particle during
// resampling. This is the canonical representative of the ~94%-scalar
// ROI's real instruction mix (double load, fcompare, conditional branch,
// early break) -- used to check whether VP++'s flat, per-opcode scalar
// timing model (no load-use-hazard or branch-misprediction modeling)
// matches real CVA6 RTL for this exact pattern.
#include <stdint.h>
#include "printf.h"
#include "runtime.h"

#define N 16

static double CDF[N] __attribute__((aligned(64)));

// Exact copy of the real findIndex() function.
static int findIndex(double *cdf, int lengthCDF, double value) {
    int index = -1;
    int x;
    for (x = 0; x < lengthCDF; x++) {
        if (cdf[x] >= value) {
            index = x;
            break;
        }
    }
    if (index == -1) {
        return lengthCDF - 1;
    }
    return index;
}

int main() {
    HW_CNT_READY;
    // Monotonically increasing CDF, matching the real algorithm's
    // invariant (a cumulative distribution function).
    for (int i = 0; i < N; i++) CDF[i] = (double)(i + 1) / (double)N;

    volatile int sink = 0;
    start_timer();
    // Call findIndex() N times with increasing target values, matching
    // the real resampling loop's u[j] sweep -- average early-exit depth
    // grows linearly across calls (0..N), same as the real distribution.
    for (int j = 0; j < N; j++) {
        double u = (double)(j + 1) / (double)N - 0.001;
        sink += findIndex(CDF, N, u);
    }
    stop_timer();

    printf("[ROI-LATENCY]: %ld cycles (sink=%d)\n", get_timer(), sink);
    printf("SUCCESS\n");
    return 0;
}
