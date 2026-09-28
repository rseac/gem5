// Disambiguates two confounds in ubench_blackscholes_lsu_only's +443%
// result vs computeUnitLoad's calibration baseline (ubench_mem_throughput):
// (1) small vl (8, vs the calibration's tested range of 16-1024)
// (2) destination-register REUSE across reps (WAW hazard), vs the
//     calibration's explicitly independent/rotating registers.
//
// Also matches blackscholes's real SEW (32, float) rather than the
// calibration's SEW64 (double).
//
// Prints 4 variants so a single RTL run separates both variables:
//   A: independent regs (v8/v9/v10/v11 rotating), vl=32 (real blackscholes vl
//      at VLEN=1024/SEW32/LMUL1 -- NUM_OPTIONS=64 strip-mines in gvl<=32
//      chunks, not the vl=8 this whole bisection chain used so far)
//   B: independent regs, vl=8 (small-vl-only confound, reuse held constant)
//   C: SAME register every rep (WAW), vl=32 (reuse-only confound)
//   D: SAME register every rep (WAW), vl=8 (matches ubench_blackscholes_lsu_only)
#include <stdint.h>
#include "printf.h"
#include "runtime.h"

#define REPS 16
static float g_buf[64] __attribute__((aligned(4 * NR_LANES * NR_CLUSTERS)));

static void indep_regs(int64_t vl) {
  asm volatile("vsetvli zero, %0, e32, m1, ta, ma" ::"r"(vl));
  float* p = g_buf;
  start_timer();
  for (int r = 0; r < REPS; r++) {
    asm volatile("vle32.v v8,  (%0)" ::"r"(p));
    asm volatile("vle32.v v9,  (%0)" ::"r"(p));
    asm volatile("vle32.v v10, (%0)" ::"r"(p));
    asm volatile("vle32.v v11, (%0)" ::"r"(p));
  }
  stop_timer();
  printf("[VL]: %ld [OP]: indep [ROI-LATENCY]: %ld cycles\n", vl, get_timer());
}

static void waw_reuse(int64_t vl) {
  asm volatile("vsetvli zero, %0, e32, m1, ta, ma" ::"r"(vl));
  float* p = g_buf;
  start_timer();
  for (int r = 0; r < REPS; r++) {
    asm volatile("vle32.v v8, (%0)" ::"r"(p));
    asm volatile("vle32.v v8, (%0)" ::"r"(p));
    asm volatile("vle32.v v8, (%0)" ::"r"(p));
    asm volatile("vle32.v v8, (%0)" ::"r"(p));
  }
  stop_timer();
  printf("[VL]: %ld [OP]: waw [ROI-LATENCY]: %ld cycles\n", vl, get_timer());
}

int main() {
  HW_CNT_READY;
  for (int i = 0; i < 64; i++) g_buf[i] = (float)i;

  indep_regs(32);  // A
  indep_regs(8);   // B
  waw_reuse(32);   // C
  waw_reuse(8);    // D

  printf("SUCCESS\n");
  return 0;
}
