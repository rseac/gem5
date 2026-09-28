#if defined(__GNUC__) || defined(__clang__)
#pragma GCC optimize ("no-tree-vectorize")
#endif

#include <stdint.h>
#include <string.h>
#include "kernel/particlefilter.h"
#include "runtime.h"
#include "util.h"

#ifdef SPIKE
#include <stdio.h>
#elif defined ARA_LINUX
#include <stdio.h>
#else
#include "printf.h"
#endif

float weights[N_PARTICLES] __attribute__((aligned(4 * NR_LANES * NR_CLUSTERS), section(".l2")));
float arrayX[N_PARTICLES] __attribute__((aligned(4 * NR_LANES * NR_CLUSTERS), section(".l2")));
float arrayY[N_PARTICLES] __attribute__((aligned(4 * NR_LANES * NR_CLUSTERS), section(".l2")));
float likelihood[N_PARTICLES] __attribute__((aligned(4 * NR_LANES * NR_CLUSTERS), section(".l2")));
float xj[N_PARTICLES] __attribute__((aligned(4 * NR_LANES * NR_CLUSTERS), section(".l2")));
float yj[N_PARTICLES] __attribute__((aligned(4 * NR_LANES * NR_CLUSTERS), section(".l2")));

int main() {
    HW_CNT_READY;
    int64_t e2e_start_cycles = get_cycle_count();

    printf("\n===================================\n");
    printf("=  RIVEC PARTICLEFILTER BENCHMARK =\n");
    printf("===================================\n\n");
    printf("Executing Particle Filter (N=%d particles, %d steps)...\n", N_PARTICLES, N_STEPS);

    particlefilter_init(N_PARTICLES, weights, arrayX, arrayY);

    start_timer();
    particlefilter_vector(N_PARTICLES, N_STEPS, weights, arrayX, arrayY, likelihood, xj, yj);
    stop_timer();

    int64_t roi_cycles = get_timer();
    printf("[ROI-LATENCY]: %ld cycles\n", roi_cycles);
    printf("[hw-cycles]: %ld\n", roi_cycles);

    printf("Verifying weights...\n");
    int err = particlefilter_verify(N_PARTICLES, weights);
    if (err != 0) {
        printf("Verification FAILED.\n");
        return 1;
    } else {
        printf("Passed.\n");
        printf("SUCCESS\n");
    }

    asm volatile("fence");
    int64_t e2e_total_cycles = get_cycle_count() - e2e_start_cycles;
    printf("[E2E-LATENCY]: %ld cycles\n", e2e_total_cycles);

    return 0;
}

