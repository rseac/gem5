#pragma GCC optimize ("no-tree-vectorize")
#include <stdint.h>
#include <string.h>
#include "kernel/streamcluster.h"
#include "runtime.h"
#include "util.h"

#ifdef SPIKE
#include <stdio.h>
#elif defined ARA_LINUX
#include <stdio.h>
#else
#include "printf.h"
#endif

float points[NUM_POINTS * DIM_POINTS] __attribute__((aligned(4 * NR_LANES * NR_CLUSTERS), section(".l2")));
float centers[NUM_CENTERS * DIM_POINTS] __attribute__((aligned(4 * NR_LANES * NR_CLUSTERS), section(".l2")));
float min_dists[NUM_POINTS] __attribute__((aligned(4 * NR_LANES * NR_CLUSTERS), section(".l2")));
int assignments[NUM_POINTS] __attribute__((aligned(4 * NR_LANES * NR_CLUSTERS), section(".l2")));

int main() {
    HW_CNT_READY;
    int64_t e2e_start_cycles = get_cycle_count();

    printf("\n===================================\n");
    printf("=  RIVEC STREAMCLUSTER BENCHMARK  =\n");
    printf("===================================\n\n");
    printf("Clustering %d points (dim=%d, %d centers)...\n", NUM_POINTS, DIM_POINTS, NUM_CENTERS);

    streamcluster_init(NUM_POINTS, DIM_POINTS, points, centers);

    start_timer();
    streamcluster_vector(NUM_POINTS, DIM_POINTS, NUM_CENTERS, points, centers, min_dists, assignments);
    stop_timer();

    int64_t roi_cycles = get_timer();
    printf("[ROI-LATENCY]: %ld cycles\n", roi_cycles);
    printf("[hw-cycles]: %ld\n", roi_cycles);

    printf("Verifying clusters...\n");
    int err = streamcluster_verify(NUM_POINTS, min_dists);
    if (err != 0) {
        printf("Verification FAILED at point %d.\n", err);
    } else {
        printf("Passed.\n");
        printf("SUCCESS\n");
    }

    asm volatile("fence");
    int64_t e2e_total_cycles = get_cycle_count() - e2e_start_cycles;
    printf("[E2E-LATENCY]: %ld cycles\n", e2e_total_cycles);

    return 0;
}
