#pragma GCC optimize ("no-tree-vectorize")
#include <stdint.h>
#include <string.h>
#include "kernel/swaptions.h"
#include "runtime.h"
#include "util.h"

#ifdef SPIKE
#include <stdio.h>
#elif defined ARA_LINUX
#include <stdio.h>
#else
#include "printf.h"
#endif

double swaption_yields[NUM_SWAPTIONS] __attribute__((aligned(4 * NR_LANES * NR_CLUSTERS), section(".l2")));
double swaption_vols[NUM_SWAPTIONS] __attribute__((aligned(4 * NR_LANES * NR_CLUSTERS), section(".l2")));
double sim_prices[NUM_SWAPTIONS] __attribute__((aligned(4 * NR_LANES * NR_CLUSTERS), section(".l2")));

int main() {
    HW_CNT_READY;
    int64_t e2e_start_cycles = get_cycle_count();

    printf("\n===================================\n");
    printf("=   RIVEC SWAPTIONS BENCHMARK     =\n");
    printf("===================================\n\n");
    printf("Pricing %d interest rate swaptions...\n", NUM_SWAPTIONS);

    swaptions_init(NUM_SWAPTIONS, swaption_yields, swaption_vols);

    start_timer();
    swaptions_vector(NUM_SWAPTIONS, swaption_yields, swaption_vols, sim_prices);
    stop_timer();

    int64_t roi_cycles = get_timer();
    printf("[ROI-LATENCY]: %ld cycles\n", roi_cycles);
    printf("[hw-cycles]: %ld\n", roi_cycles);

    printf("Verifying swaption prices...\n");
    int err = swaptions_verify(NUM_SWAPTIONS, sim_prices);
    if (err != 0) {
        printf("Verification FAILED at swaption %d.\n", err);
    } else {
        printf("Passed.\n");
        printf("SUCCESS\n");
    }

    asm volatile("fence");
    int64_t e2e_total_cycles = get_cycle_count() - e2e_start_cycles;
    printf("[E2E-LATENCY]: %ld cycles\n", e2e_total_cycles);

    return 0;
}
