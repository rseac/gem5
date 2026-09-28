// Second-level bisection of the blackscholes outer-math gap (-37.4% @2L,
// -30.3% @4L, -26.8% @8L found in ubench_blackscholes_outer_only). That probe
// mixed two real hardware FU categories (vfdiv, vfsqrt) with two long
// software-approximation chains (EXP, LOG). This probe isolates ONLY the
// real hardware div/sqrt chain -- no EXP, no LOG -- to test whether the
// outer-math gap is a simple flat-latency miscalibration of vfdiv/vfsqrt
// (fixable like Ara's earlier FDIV/FSQRT split) or lives elsewhere.
#include <stdint.h>
#include "printf.h"
#include "runtime.h"
#include "vector_defines.h"

typedef float fptype;
#define REPS 16

static void DivSqrtOnly(fptype* OptionPrice, fptype* sptprice, fptype* strike,
                         fptype* rate, fptype* volatility, fptype* time,
                         unsigned long int gvl) {
  _MMR_f32 xStockPrice, xStrikePrice, xVolatility, xTime, xSqrtTime, xDen, xD1;

  xStrikePrice = _MM_LOAD_f32(strike, gvl);
  xStockPrice = _MM_LOAD_f32(sptprice, gvl);
  xStrikePrice = _MM_DIV_f32(xStockPrice, xStrikePrice, gvl);  // DIV #1

  xVolatility = _MM_LOAD_f32(volatility, gvl);
  xTime = _MM_LOAD_f32(time, gvl);
  xSqrtTime = _MM_SQRT_f32(xTime, gvl);                        // SQRT

  xDen = _MM_MUL_f32(xVolatility, xSqrtTime, gvl);
  xD1 = _MM_DIV_f32(xStrikePrice, xDen, gvl);                  // DIV #2 (dependent on DIV #1)

  xD1 = _MM_ADD_f32(xD1, xSqrtTime, gvl);
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
    DivSqrtOnly(price, sptprice, strike, rate, volatility, otime, gvl);
  }
  stop_timer();

  printf("[ROI-LATENCY]: %ld cycles\n", get_timer());
  printf("SUCCESS\n");
  return 0;
}
