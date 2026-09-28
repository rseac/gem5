// Bisection probe for the blackscholes -40%+ gap found in ubench_blackscholes_cndf.
// That probe showed CNDF_SIMD alone (16 independent calls) matches RTL within
// -7%, but the REAL BlkSchlsEqEuroNoDiv_vector (outer LOG/SQRT/DIV/EXP math
// feeding two dependent CNDF_SIMD calls) shows -40%+ underestimate.
//
// This probe reproduces ONLY the outer math (LOG, SQRT, MUL, DIV, EXP chain
// that produces xD1/xD2) and stores xD1/xD2 directly, skipping CNDF_SIMD
// entirely. If this alone reproduces a large gap, the bug is in how the
// outer-math FU categories (FDIV/FSQRT/FCONV/EXP-approx) are costed. If this
// matches RTL closely (like CNDF alone did), the bug is specifically in the
// CROSS-CATEGORY hazard/chaining when outer-math output feeds into CNDF.
#include <stdint.h>
#include "printf.h"
#include "runtime.h"
#include "vector_defines.h"

typedef float fptype;
#define REPS 16

static void OuterMathOnly(fptype* OptionPrice, fptype* sptprice, fptype* strike,
                           fptype* rate, fptype* volatility, fptype* time,
                           unsigned long int gvl) {
  _MMR_f32 xStockPrice, xStrikePrice, xRiskFreeRate, xVolatility, xTime;
  _MMR_f32 xSqrtTime, xLogTerm, xD1, xD2, xPowerTerm, xDen;
  _MMR_f32 xRatexTime, xFutureValueX;

  xStrikePrice = _MM_LOAD_f32(strike, gvl);
  xStockPrice = _MM_LOAD_f32(sptprice, gvl);
  xStrikePrice = _MM_DIV_f32(xStockPrice, xStrikePrice, gvl);
  xLogTerm = _MM_LOG_f32(xStrikePrice, gvl);
  xRiskFreeRate = _MM_LOAD_f32(rate, gvl);
  xVolatility = _MM_LOAD_f32(volatility, gvl);
  xTime = _MM_LOAD_f32(time, gvl);
  xSqrtTime = _MM_SQRT_f32(xTime, gvl);
  xRatexTime = _MM_MUL_f32(xRiskFreeRate, xTime, gvl);
  xRatexTime = _MM_VFSGNJN_f32(xRatexTime, xRatexTime, gvl);

  xFutureValueX = _MM_EXP_f32(xRatexTime, gvl);
  xPowerTerm = _MM_MUL_f32(xVolatility, xVolatility, gvl);
  xPowerTerm = _MM_MUL_f32(xPowerTerm, _MM_SET_f32(0.5, gvl), gvl);
  xD1 = _MM_ADD_f32(xRiskFreeRate, xPowerTerm, gvl);

  xD1 = _MM_MADD_f32(xD1, xTime, xLogTerm, gvl);

  xDen = _MM_MUL_f32(xVolatility, xSqrtTime, gvl);
  xD1 = _MM_DIV_f32(xD1, xDen, gvl);
  xD2 = _MM_SUB_f32(xD1, xDen, gvl);

  // No CNDF_SIMD calls -- store D1+D2+FutureValueX directly so the compiler
  // can't dead-code-eliminate any of the outer-math chain.
  xD1 = _MM_ADD_f32(xD1, xD2, gvl);
  xD1 = _MM_ADD_f32(xD1, xFutureValueX, gvl);
  _MM_STORE_f32(OptionPrice, xD1, gvl);
}

static fptype sptprice[8] __attribute__((aligned(64)));
static fptype strike[8] __attribute__((aligned(64)));
static fptype rate[8] __attribute__((aligned(64)));
static fptype volatility[8] __attribute__((aligned(64)));
static fptype otime[8] __attribute__((aligned(64)));
static fptype price[8] __attribute__((aligned(64)));

int main() {
  HW_CNT_READY;
  for (int i = 0; i < 8; i++) {
    sptprice[i] = 40.0f + i;
    strike[i] = 38.0f + i;
    rate[i] = 0.03f;
    volatility[i] = 0.25f;
    otime[i] = 1.0f;
  }
  unsigned long gvl = 8;

  start_timer();
  for (int r = 0; r < REPS; r++) {
    OuterMathOnly(price, sptprice, strike, rate, volatility, otime, gvl);
  }
  stop_timer();

  printf("[ROI-LATENCY]: %ld cycles\n", get_timer());
  printf("SUCCESS\n");
  return 0;
}
