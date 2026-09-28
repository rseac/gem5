/*************************************************************************
** * * * * * * * * *  MATRIX MULTIPLICATION * * * * * * * * * * * * * * **
**************************************************************************/

#include <stdlib.h>
#include <stdio.h>
#include "printf.h"
#include <math.h>
#include <assert.h>
#include <stdbool.h>

#include "common/riscv_util.h"
#include "runtime.h"


#define DATA_TYPE
typedef double data_t;

#ifndef MATMUL_SIZE
int read_matrix_dimensions(FILE *file, size_t *M, size_t *K, size_t *N);
void read_vector(FILE *file, double *vector, size_t size, size_t rowSize);
#endif
extern bool compare( size_t dm, size_t dn, data_t *a , data_t *b) ;
#ifdef USE_RISCV_VECTOR
extern void matrixmul_intrinsics(data_t *a, data_t *b, data_t *c, int n, int m, int p) ;
#else // !USE_RISCV_VECTOR
extern void matmul_serial(data_t *a, data_t *b, data_t *c, int n, int m, int p);
#endif


int main (int argc, char **argv)
{
    
    HW_CNT_READY;
    int64_t e2e_start_cycles = get_cycle_count();

#ifdef MATMUL_SIZE
    // Baremetal / fixed-size build: no filesystem is available under the
    // Verilator RTL testbench, so instead of reading an input file we
    // synthesize square MxKxN matrices of the compile-time size and
    // compute a reference on-device. Set via -DMATMUL_SIZE=<n> (rivec.mk).
    size_t M = MATMUL_SIZE, K = MATMUL_SIZE, N = MATMUL_SIZE;
    printf("Matrix Dimensions (fixed): M %zu, K %zu, N %zu \n", M, K, N);

    static data_t M1_static[4096*4096];
    static data_t M2_static[4096*4096];
    static data_t result_static[4096*4096];
    static data_t reference_static[4096*4096];
    data_t *M1 = M1_static;
    data_t *M2 = M2_static;
    data_t *result = result_static;
    data_t *reference = reference_static;


    // Deterministic, simple fill so results are reproducible.
    for (size_t i = 0; i < M*K; i++) M1[i] = (data_t)((i % 7) + 1);
    for (size_t i = 0; i < K*N; i++) M2[i] = (data_t)((i % 5) + 1);

    // Reference (serial) matmul: reference[m][n] = sum_k M1[m][k]*M2[k][n].
    for (size_t m = 0; m < M; m++) {
        for (size_t n = 0; n < N; n++) {
            data_t acc = 0;
            for (size_t k = 0; k < K; k++)
                acc += M1[m*K + k] * M2[k*N + n];
            reference[m*N + n] = acc;
        }
    }
#else
    if (argc != 2){
        printf("Usage:\n\t%s <inputFile>\n", argv[0]);
        exit(1);
    }

    //Read input data from file
    char *inputFile = argv[1];
    FILE *file = fopen(inputFile, "r");
    if(file == NULL) {
      printf("ERROR: Unable to open file `%s'.\n", inputFile);
      exit(1);
    }

    size_t M, K, N;
    char line[16];

    if (read_matrix_dimensions(file, &M, &K, &N)) {
        printf("Error reading the matrix dimensions.\n");
    } else{
        printf("Matrix Dimensions: M %zu, K %zu, N %zu \n", M, K, N);
    }

    static data_t M1_static[4096*4096];
    static data_t M2_static[4096*4096];
    static data_t result_static[4096*4096];
    static data_t reference_static[4096*4096];
    data_t *M1 = M1_static;
    data_t *M2 = M2_static;
    data_t *result = result_static;
    data_t *reference = reference_static;


    // Read Matrix A
    read_vector(file, M1, M*K, K);

    // Read Matrix B
    fgets(line, sizeof(line), file);  // Read the blank line
    read_vector(file, M2, K*N, N);

    // Read Matrix Reference
    fgets(line, sizeof(line), file);  // Read the blank line
    read_vector(file, reference, M*N, N);

    fclose(file);
#endif

    //**************************************************

#ifdef USE_RISCV_VECTOR

    
    start_timer();
    matrixmul_intrinsics(M1, M2, result, N, M, K);
    stop_timer();

    printf("matrixmul_intrinsics done\n");

#else // !USE_RISCV_VECTOR


    start_timer();
    matmul_serial(M1,M2,result, N, M, K);
    stop_timer();

    printf("matmul_serial done\n");

#endif

    asm volatile("fence");
    int64_t roi_cycles = get_timer();
    printf("[ROI-LATENCY]: %ld cycles\n", roi_cycles);
    printf("[hw-cycles]: %ld\n", roi_cycles);
    int64_t e2e_total_cycles = get_cycle_count() - e2e_start_cycles;
    printf("[E2E-LATENCY]: %ld cycles\n", e2e_total_cycles);

    if(compare(M, N, result, reference)){
        printf("Verification failed!\n");
        return 1;
    } else {
        printf("Verification passed!\n");
    }

    free(M1);
    free(M2);
    free(result);
    free(reference);

    return 0;
}

#ifndef MATMUL_SIZE
void read_vector(FILE *file, double *vector, size_t size, size_t rowSize) {
    double *ptr = vector;
    int index = 0;
    double value;

    while (index < size) {
        // Read ELEMENTS_PER_LINE values from each line
        for (int i = 0; i < rowSize && index < size ; i++) {
            if (fscanf(file, "%lf", &value) != 1) {
                fprintf(stderr, "Error reading value\n");
                exit(EXIT_FAILURE);
            }
            *(ptr + index++) = value;
        }
        // Handle the newline character if present
        int ch = fgetc(file);
        if (ch != '\n' && ch != EOF) {
            ungetc(ch, file);
        }
    }
}

int read_matrix_dimensions(FILE *file, size_t *M, size_t *K, size_t *N) {
    char line[100];  // Buffer to store the line read from the file

    // Read a line from the file
    if (fgets(line, sizeof(line), file) != NULL) {
        // Parse the dimensions using sscanf
        if (sscanf(line, "%zd %zd %zd", M, K, N) != 0) {
            return 0;  // Successfully read all dimensions
        } else {
            return 1;  // Error parsing the dimensions
        }
    } else {
        return 1;  // Error reading the line
    }
}
#endif
