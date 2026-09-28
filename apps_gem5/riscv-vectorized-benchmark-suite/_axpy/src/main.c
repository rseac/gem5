/*************************************************************************
* Axpy Kernel
* Author: Jesus Labarta
* Barcelona Supercomputing Center
*************************************************************************/

#include <stdlib.h>
#include <stdio.h>
#include <math.h>
#include <assert.h>
#include "utils.h"

#include "printf.h"
#include "common/riscv_util.h"
#include "runtime.h"

/*************************************************************************/

#ifndef USE_RISCV_VECTOR
    void axpy_serial(double a, double *dx, double *dy, int n);
#else
    void axpy_vector(double a, double *dx, double *dy, int n);
#endif

extern char end;
extern int n;

int main()
{
    HW_CNT_READY;
    int64_t e2e_start_cycles = get_cycle_count();

    printf("Running AXPY VL=%d with AraXL config L=%d C=%d\n", n, NR_LANES, NR_CLUSTERS);

    // /* Allocate the source and result vectors */
    double *dx     = (double*)baremetal_malloc(n*sizeof(double));
    double *dy     = (double*)baremetal_malloc(n*sizeof(double));

    double a=1.53;
    init_vector(dx, n, 1.83);
    init_vector(dy, n, 2.22);

    double reference = capture_ref_result(a, dx, dy, n);

#ifndef USE_RISCV_VECTOR
    axpy_serial(a, dx, dy, n);
#else
    start_timer();
    axpy_vector(a, dx, dy, n);
    stop_timer();
#endif

    int64_t cycles = get_timer();
    printf("[ROI-LATENCY]: %ld cycles\n", cycles);
    printf("[sw-cycles] %ld\n", cycles);

    test_result(dy, reference, n);

    asm volatile("fence");
    int64_t e2e_total_cycles = get_cycle_count() - e2e_start_cycles;
    printf("[E2E-LATENCY]: %ld cycles\n", e2e_total_cycles);

    return 0;
}
