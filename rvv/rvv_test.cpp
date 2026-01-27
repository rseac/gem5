// rvv_test.c
#include <stdint.h>
#include <stdio.h>

//__attribute__((optimize("tree-vectorize")))
uint64_t vectorized_loop(uint64_t* A, uint64_t* B, uint64_t* C, const uint64_t& size)
{
    uint64_t r = 0;
    for (uint64_t k = 0; k < size; k++)
        C[k] += A[k] + B[k];
    for (uint64_t k = 0; k < size; k++)
        r += C[k];
    return r;
}

int main(int argc, char* argv[])
{
    volatile const int SIZE = 50;

    uint64_t A[SIZE];
    uint64_t B[SIZE];
    uint64_t C[SIZE];

    for (uint64_t i = 0; i < SIZE; i++) {
        A[i] = i;
        B[i] = 17 + i;
        C[i] = 0;
    }

    uint64_t r = vectorized_loop(A, B, C, SIZE);

    printf("%lu\n", r);

    return 0;
}
