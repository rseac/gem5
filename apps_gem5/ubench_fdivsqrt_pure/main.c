// Cleaner isolation of VMFPU_FDIV/VMFPU_FSQRT than ubench_blackscholes_divsqrt_only,
// which still mixed in 4 LSU loads/rep (avg ~19.88 cyc/load) that dominated its
// total cost and confounded the div/sqrt-specific read. This probe uses only
// _MM_SET_f32 (immediate broadcast, no memory) for operands, so every vector
// FU instruction in the ROI is either FDIV, FSQRT, or a cheap SET/glue op --
// isolating whether Ara's flat 7-cycle SEW32 FDIV/FSQRT calibration
// (from ubench_fdiv_chain_hazards, a GENUINELY dependent chain) holds when
// applied to independent, register-reused repetitions of a short 2-div+1-sqrt
// dependent chain (the actual pattern blackscholes exercises).
#include <stdint.h>
#include "printf.h"
#include "runtime.h"
#include "vector_defines.h"

typedef float fptype;
#define REPS 64

static void DivSqrtPure(fptype* out, unsigned long int gvl) {
  _MMR_f32 xA = _MM_SET_f32(40.0f, gvl);
  _MMR_f32 xB = _MM_SET_f32(38.0f, gvl);
  _MMR_f32 xC = _MM_SET_f32(1.0f, gvl);
  _MMR_f32 xRatio = _MM_DIV_f32(xA, xB, gvl);      // DIV #1
  _MMR_f32 xSqrtC = _MM_SQRT_f32(xC, gvl);         // SQRT
  _MMR_f32 xD1 = _MM_DIV_f32(xRatio, xSqrtC, gvl); // DIV #2 (dependent on DIV#1 & SQRT)
  _MM_STORE_f32(out, xD1, gvl);
}

static fptype price[8] __attribute__((aligned(64)));

int main() {
  HW_CNT_READY;
  unsigned long gvl = 8;

  start_timer();
  for (int r = 0; r < REPS; r++) {
    DivSqrtPure(price, gvl);
  }
  stop_timer();

  printf("[ROI-LATENCY]: %ld cycles\n", get_timer());
  printf("SUCCESS\n");
  return 0;
}
