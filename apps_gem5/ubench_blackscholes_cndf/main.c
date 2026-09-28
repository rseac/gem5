// Faithful reproduction of blackscholes's REAL outer computation
// (BlkSchlsEqEuroNoDiv_vector, RiVEC _blackscholes/src/main.cpp) -- not
// just the CNDF_SIMD chain in isolation. An earlier, CNDF-only version of
// this probe (16 back-to-back independent CNDF_SIMD calls) matched RTL
// within -7%, yet the real benchmark (which calls CNDF_SIMD only TWICE,
// surrounded by LOG/SQRT/EXP/DIV outer-function math) shows -40%+ error
// after the lookupRTL occupancy fix -- so the gap must be in how the two
// CNDF_SIMD calls interact with each other and the surrounding code, not
// in the CNDF chain itself. This probe reproduces the whole function.
#include <stdint.h>
#include "printf.h"
#include "runtime.h"
#include "vector_defines.h"

typedef float fptype;
#define inv_sqrt_2xPI 0.39894228040143270286
#define REPS 16

static _MMR_f32 CNDF_SIMD(_MMR_f32 xInput, unsigned long int gvl) {
  _MMR_f32 xNPrimeofX, xK2, xK2_2, xK2_3, xK2_4, xK2_5;
  _MMR_f32 xLocal, xLocal_1, xLocal_2, xFinal, xVLocal_2, expValues;
  _MMR_MASK_i32 xMask;
  _MMR_f32 xOne = _MM_SET_f32(1.0, gvl);

  xVLocal_2 = _MM_SET_f32(0.0, gvl);
  xMask = _MM_VFLT_f32(xInput, xVLocal_2, gvl);
  xInput = _MM_VFSGNJX_f32(xInput, xInput, gvl);

  expValues = _MM_MUL_f32(xInput, xInput, gvl);
  expValues = _MM_MUL_f32(expValues, _MM_SET_f32(-0.5, gvl), gvl);
  xNPrimeofX = _MM_EXP_f32(expValues, gvl);
  xNPrimeofX = _MM_MUL_f32(xNPrimeofX, _MM_SET_f32(inv_sqrt_2xPI, gvl), gvl);

  xK2 = _MM_MADD_f32(_MM_SET_f32(0.2316419, gvl), xInput, xOne, gvl);
  xK2 = _MM_DIV_f32(xOne, xK2, gvl);
  xK2_2 = _MM_MUL_f32(xK2, xK2, gvl);
  xK2_3 = _MM_MUL_f32(xK2_2, xK2, gvl);
  xK2_4 = _MM_MUL_f32(xK2_3, xK2, gvl);
  xK2_5 = _MM_MUL_f32(xK2_4, xK2, gvl);

  xLocal_1 = _MM_MUL_f32(xK2, _MM_SET_f32(0.319381530, gvl), gvl);
  xLocal_2 = _MM_MUL_f32(xK2_2, _MM_SET_f32(-0.356563782, gvl), gvl);
  xLocal_2 = _MM_MACC_f32(xLocal_2, xK2_3, _MM_SET_f32(1.781477937, gvl), gvl);
  xLocal_2 = _MM_MACC_f32(xLocal_2, xK2_4, _MM_SET_f32(-1.821255978, gvl), gvl);
  xLocal_2 = _MM_MACC_f32(xLocal_2, xK2_5, _MM_SET_f32(1.330274429, gvl), gvl);

  xLocal_1 = _MM_ADD_f32(xLocal_2, xLocal_1, gvl);
  xLocal = _MM_MUL_f32(xLocal_1, xNPrimeofX, gvl);
  xFinal = _MM_SUB_f32(_MM_SET_f32(1.0, gvl), xLocal, gvl);
  xFinal = _MM_MERGE_f32(xFinal, xLocal, xMask, gvl);
  return xFinal;
}

static void BlkSchlsEqEuroNoDiv_vector(fptype* OptionPrice, int numOptions,
                                        fptype* sptprice, fptype* strike,
                                        fptype* rate, fptype* volatility,
                                        fptype* time, int* otype,
                                        unsigned long int gvl) {
  _MMR_f32 xStockPrice, xStrikePrice, xRiskFreeRate, xVolatility, xTime;
  _MMR_f32 xSqrtTime, xLogTerm, xD1, xD2, xPowerTerm, xDen;
  _MMR_f32 xRatexTime, xFutureValueX;
  _MMR_MASK_i32 xMask;
  _MMR_i32 xOtype, xZero;
  _MMR_f32 xOptionPrice, xOptionPrice1, xOptionPrice2, xfXd1, xfXd2;

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

  xfXd1 = CNDF_SIMD(xD1, gvl);
  xfXd2 = CNDF_SIMD(xD2, gvl);

  xStrikePrice = _MM_LOAD_f32(strike, gvl);
  xFutureValueX = _MM_MUL_f32(xFutureValueX, xStrikePrice, gvl);

  xOtype = _MM_LOAD_i32(otype, gvl);
  xZero = _MM_SET_i32(0, gvl);
  xMask = _MM_VMSEQ_i32(xZero, xOtype, gvl);
  xfXd1 = _MM_MERGE_f32(_MM_SUB_f32(_MM_SET_f32(1.0, gvl), xfXd1, gvl), xfXd1, xMask, gvl);
  xStockPrice = _MM_LOAD_f32(sptprice, gvl);
  xOptionPrice1 = _MM_MUL_f32(xStockPrice, xfXd1, gvl);
  xfXd2 = _MM_MERGE_f32(_MM_SUB_f32(_MM_SET_f32(1.0, gvl), xfXd2, gvl), xfXd2, xMask, gvl);
  xOptionPrice2 = _MM_MUL_f32(xFutureValueX, xfXd2, gvl);
  xOptionPrice = _MM_SUB_f32(xOptionPrice2, xOptionPrice1, gvl);
  xOptionPrice = _MM_VFSGNJX_f32(xOptionPrice, xOptionPrice, gvl);
  _MM_STORE_f32(OptionPrice, xOptionPrice, gvl);
}

static fptype sptprice[8] __attribute__((aligned(64)));
static fptype strike[8] __attribute__((aligned(64)));
static fptype rate[8] __attribute__((aligned(64)));
static fptype volatility[8] __attribute__((aligned(64)));
static fptype otime[8] __attribute__((aligned(64)));
static int otype[8] __attribute__((aligned(64)));
static fptype price[8] __attribute__((aligned(64)));

int main() {
  HW_CNT_READY;
  for (int i = 0; i < 8; i++) {
    sptprice[i] = 40.0f + i;
    strike[i] = 38.0f + i;
    rate[i] = 0.03f;
    volatility[i] = 0.25f;
    otime[i] = 1.0f;
    otype[i] = i % 2;
  }
  unsigned long gvl = 8;

  start_timer();
  for (int r = 0; r < REPS; r++) {
    BlkSchlsEqEuroNoDiv_vector(price, 8, sptprice, strike, rate, volatility,
                               otime, otype, gvl);
  }
  stop_timer();

  printf("[ROI-LATENCY]: %ld cycles\n", get_timer());
  printf("SUCCESS\n");
  return 0;
}
