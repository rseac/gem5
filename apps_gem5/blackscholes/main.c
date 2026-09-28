#pragma GCC optimize ("no-tree-vectorize")
#include <stdint.h>
#include <string.h>
#include "kernel/blackscholes.h"
#include "runtime.h"
#include "util.h"

#ifdef SPIKE
#include <stdio.h>
#elif defined ARA_LINUX
#include <stdio.h>
#else
#include "printf.h"
#endif

#define NUM_OPTIONS 64

extern fptype sptprice[NUM_OPTIONS] __attribute__((aligned(4 * NR_LANES * NR_CLUSTERS), section(".l2")));
extern fptype strike[NUM_OPTIONS] __attribute__((aligned(4 * NR_LANES * NR_CLUSTERS), section(".l2")));
extern fptype rate[NUM_OPTIONS] __attribute__((aligned(4 * NR_LANES * NR_CLUSTERS), section(".l2")));
extern fptype volatility[NUM_OPTIONS] __attribute__((aligned(4 * NR_LANES * NR_CLUSTERS), section(".l2")));
extern fptype otime[NUM_OPTIONS] __attribute__((aligned(4 * NR_LANES * NR_CLUSTERS), section(".l2")));
extern int otype[NUM_OPTIONS] __attribute__((aligned(4 * NR_LANES * NR_CLUSTERS), section(".l2")));
extern fptype prices[NUM_OPTIONS] __attribute__((aligned(4 * NR_LANES * NR_CLUSTERS), section(".l2")));

int main() {
    HW_CNT_READY;
    int64_t e2e_start_cycles = get_cycle_count();

    printf("\n===================================\n");
    printf("=  RIVEC BLACKSCHOLES BENCHMARK   =\n");
    printf("===================================\n\n");
    printf("Evaluating %d European options...\n", NUM_OPTIONS);

    start_timer();
    blackscholes_vector(NUM_OPTIONS, sptprice, strike, rate, volatility, otime, otype, prices);
    stop_timer();

    int64_t roi_cycles = get_timer();
    printf("[ROI-LATENCY]: %ld cycles\n", roi_cycles);
    printf("[hw-cycles]: %ld\n", roi_cycles);

    printf("Verifying options pricing...\n");
    int err = blackscholes_verify(NUM_OPTIONS, sptprice, strike, rate, volatility, otime, otype, prices);
    if (err != 0) {
        printf("Verification FAILED at option %d.\n", err);
        return err;
    } else {
        printf("Passed.\n");
        printf("SUCCESS\n");
    }

    asm volatile("fence");
    int64_t e2e_total_cycles = get_cycle_count() - e2e_start_cycles;
    printf("[E2E-LATENCY]: %ld cycles\n", e2e_total_cycles);

    return 0;
}
