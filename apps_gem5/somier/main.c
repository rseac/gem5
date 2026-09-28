#if defined(__GNUC__) || defined(__clang__)
#pragma GCC optimize ("no-tree-vectorize")
#endif

#include <stdint.h>
#include <string.h>
#include "kernel/somier.h"
#include "runtime.h"
#include "util.h"

#ifdef SPIKE
#include <stdio.h>
#elif defined ARA_LINUX
#include <stdio.h>
#else
#include "printf.h"
#endif

double X[3][SOMIER_N][SOMIER_N][SOMIER_N] __attribute__((aligned(4 * NR_LANES * NR_CLUSTERS), section(".l2")));
double V[3][SOMIER_N][SOMIER_N][SOMIER_N] __attribute__((aligned(4 * NR_LANES * NR_CLUSTERS), section(".l2")));
double A[3][SOMIER_N][SOMIER_N][SOMIER_N] __attribute__((aligned(4 * NR_LANES * NR_CLUSTERS), section(".l2")));
double F[3][SOMIER_N][SOMIER_N][SOMIER_N] __attribute__((aligned(4 * NR_LANES * NR_CLUSTERS), section(".l2")));

int main() {
    HW_CNT_READY;
    int64_t e2e_start_cycles = get_cycle_count();

    printf("\n===================================\n");
    printf("=     RIVEC SOMIER BENCHMARK      =\n");
    printf("===================================\n\n");
    printf("Solving spring-lattice mesh (N=%d, steps=%d)...\n", SOMIER_N, SOMIER_STEPS);

    somier_init(SOMIER_N, X, V, A, F);

    start_timer();
    somier_vector(SOMIER_N, SOMIER_STEPS, X, V, A, F);
    stop_timer();

    int64_t roi_cycles = get_timer();
    printf("[ROI-LATENCY]: %ld cycles\n", roi_cycles);
    printf("[hw-cycles]: %ld\n", roi_cycles);

    printf("Verifying forces...\n");
    int err = somier_verify(SOMIER_N, F);
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

