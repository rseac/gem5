// Real vectorized RiVEC blackscholes (genuine RVV compute via CNDF_SIMD /
// BlkSchlsEqEuroNoDiv_vector, GCC-portable vector_defines.h intrinsics --
// same core computation as benchmark_suite/ara/apps/riscv-vectorized-
// benchmark-suite/_blackscholes/src/main.cpp), adapted to XiangShan's
// runtime conventions, to compare against the degenerate dead-vector-load
// "port" (kernels/blackscholes) used for the official RiVEC-11 XSTop sweep.
//
// Same NUM_OPTIONS=64 problem size as the degenerate port, for a fair
// side-by-side comparison against its RTL=16915 / VP++=30133 numbers.
#include <stdint.h>
#include <riscv_vector.h>
#include "runtime.h"
#include "util.h"
#include "printf.h"
#include "vector_defines.h"

typedef float fptype;
#define NUM_OPTIONS 64
#define inv_sqrt_2xPI 0.39894228040143270286

static fptype sptprice[NUM_OPTIONS] __attribute__((aligned(4 * NR_LANES * NR_CLUSTERS), section(".l2")));
static fptype strike[NUM_OPTIONS] __attribute__((aligned(4 * NR_LANES * NR_CLUSTERS), section(".l2")));
static fptype rate[NUM_OPTIONS] __attribute__((aligned(4 * NR_LANES * NR_CLUSTERS), section(".l2")));
static fptype volatility[NUM_OPTIONS] __attribute__((aligned(4 * NR_LANES * NR_CLUSTERS), section(".l2")));
static fptype otime[NUM_OPTIONS] __attribute__((aligned(4 * NR_LANES * NR_CLUSTERS), section(".l2")));
static int otype[NUM_OPTIONS] __attribute__((aligned(4 * NR_LANES * NR_CLUSTERS), section(".l2")));
static fptype prices[NUM_OPTIONS] __attribute__((aligned(4 * NR_LANES * NR_CLUSTERS), section(".l2")));

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
                                        fptype* sp, fptype* st,
                                        fptype* rt, fptype* vol,
                                        fptype* tm, int* ot,
                                        unsigned long int gvl) {
  _MMR_f32 xStockPrice, xStrikePrice, xRiskFreeRate, xVolatility, xTime;
  _MMR_f32 xSqrtTime, xLogTerm, xD1, xD2, xPowerTerm, xDen;
  _MMR_f32 xRatexTime, xFutureValueX;
  _MMR_MASK_i32 xMask;
  _MMR_i32 xOtype, xZero;
  _MMR_f32 xOptionPrice, xOptionPrice1, xOptionPrice2, xfXd1, xfXd2;

  xStrikePrice = _MM_LOAD_f32(st, gvl);
  xStockPrice = _MM_LOAD_f32(sp, gvl);
  xStrikePrice = _MM_DIV_f32(xStockPrice, xStrikePrice, gvl);
  xLogTerm = _MM_LOG_f32(xStrikePrice, gvl);
  xRiskFreeRate = _MM_LOAD_f32(rt, gvl);
  xVolatility = _MM_LOAD_f32(vol, gvl);
  xTime = _MM_LOAD_f32(tm, gvl);
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

  xStrikePrice = _MM_LOAD_f32(st, gvl);
  xFutureValueX = _MM_MUL_f32(xFutureValueX, xStrikePrice, gvl);

  xOtype = _MM_LOAD_i32(ot, gvl);
  xZero = _MM_SET_i32(0, gvl);
  xMask = _MM_VMSEQ_i32(xZero, xOtype, gvl);
  xfXd1 = _MM_MERGE_f32(_MM_SUB_f32(_MM_SET_f32(1.0, gvl), xfXd1, gvl), xfXd1, xMask, gvl);
  xStockPrice = _MM_LOAD_f32(sp, gvl);
  xOptionPrice1 = _MM_MUL_f32(xStockPrice, xfXd1, gvl);
  xfXd2 = _MM_MERGE_f32(_MM_SUB_f32(_MM_SET_f32(1.0, gvl), xfXd2, gvl), xfXd2, xMask, gvl);
  xOptionPrice2 = _MM_MUL_f32(xFutureValueX, xfXd2, gvl);
  xOptionPrice = _MM_SUB_f32(xOptionPrice2, xOptionPrice1, gvl);
  xOptionPrice = _MM_VFSGNJX_f32(xOptionPrice, xOptionPrice, gvl);
  _MM_STORE_f32(OptionPrice, xOptionPrice, gvl);
}

int main() {
  HW_CNT_READY;
  for (int i = 0; i < NUM_OPTIONS; i++) {
    sptprice[i] = 40.0f + (i % 8);
    strike[i] = 38.0f + (i % 8);
    rate[i] = 0.03f;
    volatility[i] = 0.25f;
    otime[i] = 1.0f;
    otype[i] = i % 2;
  }

  printf("=== blackscholes_vectorized (real RVV compute, NUM_OPTIONS=%d) ===\n", NUM_OPTIONS);

  start_timer();
  int i = 0;
  while (i < NUM_OPTIONS) {
    unsigned long gvl = __riscv_vsetvl_e32m1(NUM_OPTIONS - i);
    BlkSchlsEqEuroNoDiv_vector(&prices[i], NUM_OPTIONS, &sptprice[i], &strike[i],
                               &rate[i], &volatility[i], &otime[i], &otype[i], gvl);
    i += gvl;
  }
  stop_timer();

  printf("[ROI-LATENCY]: %ld cycles\n", get_timer());
  printf("SUCCESS\n");
  return 0;
}
