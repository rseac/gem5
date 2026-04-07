// rvv_test.c
#include <stdint.h>
#include <string.h>

#include "runtime.h"
#include "util.h"

#include "../kernel/dotproduct.h"

#ifndef SPIKE
#include "printf.h"
#else
#include <stdio.h>
#endif


// Use attribute to encourage vectorization
__attribute__((optimize("tree-vectorize")))
uint64_t vectorized_loop(uint8_t* A, uint8_t* B, uint8_t* C, uint64_t size)
{
    uint64_t r = 0;
    // Loop 1: Vectorizable addition
    for (uint64_t k = 0; k < size; k++)
        C[k] = A[k] + B[k];
    
    // Loop 2: Reduction
    for (uint64_t k = 0; k < size; k++)
        r += C[k];
    return r;
}

int main(int argc, char* argv[])
{
    HW_CNT_READY;
    // Increase size to ensure we exceed throughput capacity
    volatile const int SIZE = 1024;

    uint8_t A[SIZE];
    uint8_t B[SIZE];
    uint8_t C[SIZE];

    for (uint64_t i = 0; i < SIZE; i++) {
        A[i] = (uint8_t)i;
        B[i] = (uint8_t)(17 + i);
        C[i] = 0;
    }

    start_timer();
    uint64_t r = vectorized_loop(A, B, C, SIZE);
    stop_timer();

    printf("%lu\n", r);
    uint64_t v_sw_runtime = get_timer();
  printf("[cycles]: %ld\n", v_sw_runtime);

    return 0;
}
