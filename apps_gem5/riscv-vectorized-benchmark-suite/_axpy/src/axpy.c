/*************************************************************************
* Vectorized Axpy Kernel
* Author: Jesus Labarta
* Barcelona Supercomputing Center
*************************************************************************/

#include <stdlib.h>
#include <stdio.h>
#include <math.h>
#include <assert.h>

#ifdef USE_RISCV_VECTOR
#include <riscv_vector.h>
#include "common/vector_defines.h"
#endif

#ifndef USE_RISCV_VECTOR

void axpy_serial(double a, double *dx, double *dy, int n) {
   int i;
   for (i=0; i<n; i++) {
      dy[i] += a*dx[i];
   }
}

#else

void axpy_vector(double a, double *dx, double *dy, int n) {
  int i;

#ifdef INTRINSICS
  long gvl = _MMR_VSETVL_E64M1(n);

  for (i = 0; i < n;) {
    gvl = _MMR_VSETVL_E64M1(n - i);
    vfloat64m1_t v_dx = _MM_LOAD_f64(&dx[i], gvl);
    vfloat64m1_t v_dy = _MM_LOAD_f64(&dy[i], gvl);
    vfloat64m1_t v_res = _MM_MACC_VF_f64(v_dy, a, v_dx, gvl);
    _MM_STORE_f64(&dy[i], v_res, gvl);

    i += gvl;
  }
#else
  long gvl;
  for (i = 0; i < n;) {
    asm volatile ("vsetvli %0, %1, e64, m8, ta, ma" : "=r"(gvl) : "r"(n-i));
    asm volatile ("vle64.v v8, (%0)"::"r"(&dx[i]));
    asm volatile ("vle64.v v16, (%0)"::"r"(&dy[i]));
    asm volatile ("vfmacc.vf v16, %0, v8"::"f"(a));
    asm volatile ("vse64.v v16, (%0)"::"r"(&dy[i]));
    i += gvl;
  }

#endif
}

#endif
