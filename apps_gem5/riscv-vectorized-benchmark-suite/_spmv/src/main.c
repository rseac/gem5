/*
 * Neiel Israel Leyva Santes
 * neiel.leyva@bsc.es
 * Barcelona Supercomputing Center
 *
 * SpMV Vector Implementation.
 * Sparse Matrix-Vector Multiplication (SpMV) is a mathematical operation
 * in which a sparse matrix is multiplied by a dense vector.
 *
 * Inputs:
 *      *tiny:   football.mtx
 *               M. Girvan and M. E. J. Newman, The network of American football games
 *               between Division IA colleges during regular season Fall 2000.
 *      *small:  lhr07.mtx
 *               J. Mallya, Light hydrocarbon recovery. OK if ill conditioned, from a nonlinear solver.
 *      *medium: venkat25.mtx
 *               V. Venkatakrishnan, Unstructured 2d euler solver, time step = 25.
 *      *large:  poisson3Db.mtx
 *               Comsol, Inc., 3D Poisson problem.
 *
 */

#include <stdlib.h>
#include <stdio.h>
#include <stdbool.h>
#include <math.h>
#include <stdint.h>
#include "common/riscv_util.h"
#define TOLERANCE 1e-6
#include "printf.h"
#include "runtime.h"


void spmv_intrinsics(const size_t nrows, double *a, uint64_t *ia, uint64_t *ja, double *x, double *y);
void spmv_serial(const size_t nrows, double *a, uint64_t *ia, uint64_t *ja, double *x, double *y);

extern size_t M, N, NZ;
extern uint64_t ia[] __attribute__((aligned(4 * NR_LANES * NR_CLUSTERS)));
extern uint64_t ja[] __attribute__((aligned(4 * NR_LANES * NR_CLUSTERS)));
extern double a[] __attribute__((aligned(4 * NR_LANES * NR_CLUSTERS)));
extern double x[] __attribute__((aligned(4 * NR_LANES * NR_CLUSTERS)));
extern double y[] __attribute__((aligned(4 * NR_LANES * NR_CLUSTERS)));
extern double verif[] __attribute__((aligned(4 * NR_LANES * NR_CLUSTERS)));

#if defined(__GNUC__) || defined(__clang__)
#pragma GCC optimize ("no-tree-vectorize")
#endif

int main(){

    HW_CNT_READY;
    int64_t e2e_start_cycles = get_cycle_count();

    bool verification = true;

    printf("Running SpMV with M=%ld, N=%ld, NNZ=%ld L=%d C=%d\n", M, N, NZ, NR_LANES, NR_CLUSTERS);

#ifdef USE_RISCV_VECTOR

    start_timer();
    spmv_intrinsics(M, a, ia, ja, x, y);
    stop_timer();

#else // !USE_RISCV_VECTOR
    start_timer();
    spmv_serial(M, a, ia, ja, x, y);
    stop_timer();
#endif
    asm volatile ("fence");

    int64_t roi_cycles = get_timer();
    printf("[ROI-LATENCY]: %ld cycles\n", roi_cycles);

    if(verification == true){
        for(size_t i=0; i < M ; i++){
            if (fabs(y[i] - verif[i]) > TOLERANCE) {
                printf("Verification fail \n");
                printf("%.17lf  -  %.17lf \n",y[i],verif[i]);
                return i+1;
            }
        }
        printf("Verification pass [sw-cycles] = %ld\n", roi_cycles);
        printf("SUCCESS\n");
    }

    asm volatile("fence");
    int64_t e2e_total_cycles = get_cycle_count() - e2e_start_cycles;
    printf("[E2E-LATENCY]: %ld cycles\n", e2e_total_cycles);

    printf ("done\n");

    return 0;
}

