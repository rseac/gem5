// Third-level bisection of the blackscholes outer-math gap. FDIV/FSQRT
// isolated with no memory ops (ubench_fdivsqrt_pure) showed only ~-11%
// error at 2L, much closer than the confounded ubench_blackscholes_divsqrt_only
// probe's +262% -- pointing at the LSU loads in that probe (4 independent-
// register loads/rep, gvl=8, e32, tiny working set, register-reused across
// reps) as the real dominant source of error, not FDIV/FSQRT. This probe
// isolates ONLY the loads -- no div, no sqrt, no FPU compute at all besides
// what's needed to prevent dead-code elimination -- to test that directly.
#include <stdint.h>
#include "printf.h"
#include "runtime.h"
#include "vector_defines.h"

typedef float fptype;
#define REPS 1

static void LsuOnly(fptype* out, fptype* sptprice, fptype* strike,
                     fptype* rate, fptype* volatility, unsigned long int gvl) {
  _MMR_f32 xA = _MM_LOAD_f32(sptprice, gvl);
  _MMR_f32 xB = _MM_LOAD_f32(strike, gvl);
  _MMR_f32 xC = _MM_LOAD_f32(rate, gvl);
  _MMR_f32 xD = _MM_LOAD_f32(volatility, gvl);
  _MMR_f32 xSum = _MM_ADD_f32(xA, xB, gvl);
  xSum = _MM_ADD_f32(xSum, xC, gvl);
  xSum = _MM_ADD_f32(xSum, xD, gvl);
  _MM_STORE_f32(out, xSum, gvl);
}

static fptype sptprice[8] __attribute__((aligned(64)));
static fptype strike[8] __attribute__((aligned(64)));
static fptype rate[8] __attribute__((aligned(64)));
static fptype volatility[8] __attribute__((aligned(64)));
static fptype price[8] __attribute__((aligned(64)));

int main() {
  HW_CNT_READY;
  for (int i = 0; i < 8; i++) {
    sptprice[i] = 40.0f + i;
    strike[i] = 38.0f + i;
    rate[i] = 0.03f;
    volatility[i] = 0.25f;
  }
  unsigned long gvl = 8;

  start_timer();
  for (int r = 0; r < REPS; r++) {
    LsuOnly(price, sptprice, strike, rate, volatility, gvl);
  }
  stop_timer();

  printf("[ROI-LATENCY]: %ld cycles\n", get_timer());
  printf("SUCCESS\n");
  return 0;
}
