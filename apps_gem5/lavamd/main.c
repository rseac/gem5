#pragma GCC optimize ("no-tree-vectorize")
// See LICENSE and LICENSE_1 for licensing terms of the original
// and vectorized version, respectively.

/*************************************************************************
 * RISC-V Vectorized Version
 * Author: Cristóbal Ramírez Lazo
 * email: cristobal.ramirez@bsc.es
 * Barcelona Supercomputing Center (2020)
 *************************************************************************/

// Modifications + Fixes to the vectorized version by:
// Matteo Perotti <mperotti@iis.ee.ethz.ch>

#include "kernel/lavamd.h"
#include "runtime.h"
#include "util.h"

#ifndef SPIKE
#include "printf.h"
#else
#include <stdio.h>
#endif

extern fp alpha;
extern uint64_t n_boxes;
extern uint64_t NUMBER_PAR_PER_BOX;

extern box_str box_cpu_mem[]
    __attribute__((aligned(4 * NR_LANES * NR_CLUSTERS), section(".l2")));
extern FOUR_VECTOR rv_cpu_mem[]
    __attribute__((aligned(4 * NR_LANES * NR_CLUSTERS), section(".l2")));
extern fp qv_cpu_mem[] __attribute__((aligned(4 * NR_LANES * NR_CLUSTERS), section(".l2")));
extern FOUR_VECTOR fv_s_cpu_mem[]
    __attribute__((aligned(4 * NR_LANES * NR_CLUSTERS), section(".l2")));
extern FOUR_VECTOR fv_v_cpu_mem[]
    __attribute__((aligned(4 * NR_LANES * NR_CLUSTERS), section(".l2")));
extern nei_str nn_mem[] __attribute__((aligned(4 * NR_LANES * NR_CLUSTERS), section(".l2")));

int main() {
  HW_CNT_READY;
  int64_t e2e_start_cycles = get_cycle_count();

  printf("\n");
  printf("=============\n");
  printf("=  LAVA-MD  =\n");
  printf("=============\n");
  printf("\n");
  printf("\n");

  int err = 0;

  printf("n_boxes = %u, NUMBER_PAR_PER_BOX = %u\n", n_boxes,
         NUMBER_PAR_PER_BOX);
#ifdef DEBUG
  printf("sizeof(box_cpu_mem[0]) = %u\n", sizeof(box_cpu_mem[0]));

  for (uint64_t i = 0; i < n_boxes; i++) {
    printf("box_cpu_mem[%d].offset = %u, while .number == %u\n", i,
           box_cpu_mem[i].offset, box_cpu_mem[i].number);
  }
#endif

  printf("Running the scalar benchmark.\n");
  kernel(alpha, n_boxes, box_cpu_mem, rv_cpu_mem, qv_cpu_mem, fv_s_cpu_mem,
         NUMBER_PAR_PER_BOX);

  printf("Running the vector benchmark.\n");
  start_timer();
  kernel_vec(alpha, n_boxes, box_cpu_mem, rv_cpu_mem, qv_cpu_mem, fv_v_cpu_mem,
             NUMBER_PAR_PER_BOX);
  stop_timer();

  int64_t roi_cycles = get_timer();
  printf("[ROI-LATENCY]: %ld cycles\n", roi_cycles);
  printf("[hw-cycles]: %ld\n", roi_cycles);

  // Check
#ifdef PSEUDO_LAVAMD
  uint64_t num_check = 4;
#else
  uint64_t num_check = n_boxes;
#endif
  for (uint64_t i = 0; i < num_check; ++i) {
    if (!similarity_check_32b(fv_s_cpu_mem[i].v, fv_v_cpu_mem[i].v,
                              THRESHOLD) ||
        !similarity_check_32b(fv_s_cpu_mem[i].x, fv_v_cpu_mem[i].x,
                              THRESHOLD) ||
        !similarity_check_32b(fv_s_cpu_mem[i].y, fv_v_cpu_mem[i].y,
                              THRESHOLD) ||
        !similarity_check_32b(fv_s_cpu_mem[i].z, fv_v_cpu_mem[i].z,
                              THRESHOLD)) {
      printf("Error at index %d: s=(%d, %d, %d, %d) v=(%d, %d, %d, %d)\n",
             (int)i,
             (int)(fv_s_cpu_mem[i].v * 1000), (int)(fv_s_cpu_mem[i].x * 1000),
             (int)(fv_s_cpu_mem[i].y * 1000), (int)(fv_s_cpu_mem[i].z * 1000),
             (int)(fv_v_cpu_mem[i].v * 1000), (int)(fv_v_cpu_mem[i].x * 1000),
             (int)(fv_v_cpu_mem[i].y * 1000), (int)(fv_v_cpu_mem[i].z * 1000));
      err = i ? i : -1;
    }
  }
  if (!err) {
    printf("Test passed. No errors found.\n");
    printf("Passed.\n");
    printf("SUCCESS\n");
  } else {
    printf("Test FAILED.\n");
  }

  asm volatile("fence");
  int64_t e2e_total_cycles = get_cycle_count() - e2e_start_cycles;
  printf("[E2E-LATENCY]: %ld cycles\n", e2e_total_cycles);

  return err;
}
