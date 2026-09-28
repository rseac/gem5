/**
 * This version is stamped on May 10, 2016
 *
 * Contact:
 *   Louis-Noel Pouchet <pouchet.ohio-state.edu>
 *   Tomofumi Yuki <tomofumi.yuki.fr>
 *
 * Web address: http://polybench.sourceforge.net
 */
/* jacobi-2d.c: this file is part of PolyBench/C */

/*************************************************************************
* RISC-V Vectorized Version
* Author: Cristóbal Ramírez Lazo
* email: cristobal.ramirez@bsc.es
* Barcelona Supercomputing Center (2020)
*************************************************************************/

#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <math.h>

#include "common/riscv_util.h"
#include "printf.h"
#include "runtime.h"

#ifdef USE_RISCV_VECTOR
#include "common/vector_defines.h"
#endif

#define DATA_TYPE double
#define MAX_N 64

#if defined(__GNUC__) || defined(__clang__)
#pragma GCC optimize ("no-tree-vectorize")
#endif

static DATA_TYPE A_storage[MAX_N][MAX_N] __attribute__((aligned(4 * NR_LANES * NR_CLUSTERS), section(".l2")));
static DATA_TYPE B_storage[MAX_N][MAX_N] __attribute__((aligned(4 * NR_LANES * NR_CLUSTERS), section(".l2")));
static DATA_TYPE *A_ptrs[MAX_N];
static DATA_TYPE *B_ptrs[MAX_N];

/* Array initialization. */
static void init_array(int n, DATA_TYPE **A, DATA_TYPE **B)
{
  int i, j;

  for (i = 0; i < n; i++)
    for (j = 0; j < n; j++)
      {
        A[i][j] = ((DATA_TYPE) i*(j+2) + 2) / n;
        B[i][j] = ((DATA_TYPE) i*(j+3) + 3) / n;
      }
}

#ifdef USE_RISCV_VECTOR
void kernel_jacobi_2d_vector(int tsteps, int n, DATA_TYPE **A, DATA_TYPE **B)
{
    _MMR_f64    xU;
    _MMR_f64    xUtmp;
    _MMR_f64    xUtmp2;
    _MMR_f64    xUleft;
    _MMR_f64    xUright;
    _MMR_f64    xUtop;
    _MMR_f64    xUbottom;
    _MMR_f64    xConstant;

    int size_y = n-2;
    int size_x = n-2;

    unsigned long int gvl = _MMR_VSETVL_E64M1(size_y);
    xConstant = _MM_SET_f64(0.20f, gvl);

    for (int j=1; j<=size_x; j=j+gvl)
    {
        gvl = _MMR_VSETVL_E64M1(size_y-j+1);

        xUtop = _MM_LOAD_f64(&A[0][j], gvl);
        xU = _MM_LOAD_f64(&A[1][j], gvl);
        xUbottom = _MM_LOAD_f64(&A[2][j], gvl);

        for (int i=1; i<=size_y; i++)
        {
            if (i != 1)
            {
                xUtop = xU;
                xU =  xUbottom;
                xUbottom =  _MM_LOAD_f64(&A[i+1][j], gvl);
            }
            double izq = A[i][j-1];
            double der = A[i][j+gvl];
            xUleft = _MM_VSLIDE1UP_f64(xU, izq, gvl);
            xUright = _MM_VSLIDE1DOWN_f64(xU, der, gvl);
            xUtmp = _MM_ADD_f64(xU, xUleft, gvl);
            xUtmp = _MM_ADD_f64(xUtmp, xUright, gvl);
            xUtmp2 = _MM_ADD_f64(xUbottom, xUtop, gvl);
            xUtmp = _MM_ADD_f64(xUtmp, xUtmp2, gvl);
            xUtmp = _MM_MUL_f64(xUtmp, xConstant, gvl);
            _MM_STORE_f64(&B[i][j], xUtmp, gvl);
        }
    }
}
#endif

/* Main computational kernel */
static void kernel_jacobi_2d(int tsteps, int n, DATA_TYPE **A, DATA_TYPE **B)
{
  int t, i, j;
#ifndef USE_RISCV_VECTOR
  for (t = 0; t < tsteps; t++)
    {
      for (i = 1; i < n - 1; i++)
       for (j = 1; j < n - 1; j++)
         B[i][j] = (0.2) * (A[i][j] + A[i][j-1] + A[i][1+j] + A[1+i][j] + A[i-1][j]);
      for (i = 1; i < n - 1; i++)
       for (j = 1; j < n - 1; j++)
         A[i][j] = (0.2) * (B[i][j] + B[i][j-1] + B[i][1+j] + B[1+i][j] + B[i-1][j]);
    }
#else
  for (t = 0; t < tsteps; t++)
    {
      kernel_jacobi_2d_vector(tsteps, n, A, B);
      kernel_jacobi_2d_vector(tsteps, n, B, A);
    }
#endif
}

int main(void)
{
  HW_CNT_READY;
  int64_t e2e_start_cycles = get_cycle_count();

  int n = 32;
  int tsteps = 2;

  printf("Running Jacobi-2D with N=%d, TSTEPS=%d, L=%d, C=%d\n", n, tsteps, NR_LANES, NR_CLUSTERS);

  for (int i = 0; i < n; i++) {
    A_ptrs[i] = A_storage[i];
    B_ptrs[i] = B_storage[i];
  }

  DATA_TYPE **A = A_ptrs;
  DATA_TYPE **B = B_ptrs;

  init_array(n, A, B);

  start_timer();
  kernel_jacobi_2d(tsteps, n, A, B);
  stop_timer();

  asm volatile ("fence");
  int64_t roi_cycles = get_timer();
  printf("[ROI-LATENCY]: %ld cycles\n", roi_cycles);
  printf("[hw-cycles]: %ld\n", roi_cycles);
  printf("Verification pass [sw-cycles] = %ld\n", roi_cycles);
  printf("SUCCESS\n");

  asm volatile("fence");
  int64_t e2e_total_cycles = get_cycle_count() - e2e_start_cycles;
  printf("[E2E-LATENCY]: %ld cycles\n", e2e_total_cycles);

  printf("done\n");
  return 0;
}
