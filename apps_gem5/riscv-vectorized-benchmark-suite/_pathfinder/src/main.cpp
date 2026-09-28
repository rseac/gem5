#include <time.h>
#include <sys/time.h>
#include <assert.h>
#include <string.h>

using namespace std;

/*************************************************************************
* RISC-V Vectorized Version
* Author: Cristóbal Ramírez Lazo
* email: cristobal.ramirez@bsc.es
* Barcelona Supercomputing Center (2020)
*************************************************************************/

#include "common/riscv_util.h"
#include "printf.h"
#include "runtime.h"

#ifdef USE_RISCV_VECTOR
#include <riscv_vector.h>
#include "common/vector_defines.h"
#endif

/************************************************************************/

//Enable RESULT_PRINT in order to see the result vector, for instruction count it should be disable
// #define RESULT_PRINT
//Enable INPUT_PRINT in order to see the input matrix, for instruction count it should be disable
//#define INPUT_PRINT

#define MAXNAMESIZE 1024 // max filename length
#define M_SEED 9
#define MAX_WEIGHT 10
#define NUM_RUNS 1

extern uint32_t rows, cols;
extern int wall[] __attribute__((aligned(4 * NR_LANES * NR_CLUSTERS), section(".l2")));
extern int result[] __attribute__((aligned(4 * NR_LANES * NR_CLUSTERS), section(".l2")));
extern int reference[] __attribute__((aligned(4 * NR_LANES * NR_CLUSTERS), section(".l2")));

void run();
void run_vector();
void output_print(int *dst, int cols);

bool compare( int cols, int* result, int* reference);

#define IN_RANGE(x, min, max)   ((x)>=(min) && (x)<=(max))
#define CLAMP_RANGE(x, min, max) x = (x<(min)) ? min : ((x>(max)) ? max : x )
#define MIN(a, b) ((a)<=(b) ? (a) : (b))

int main(int argc, char** argv)
{

#ifndef USE_RISCV_VECTOR
    run();
#else
    run_vector();
#endif

}

#ifndef USE_RISCV_VECTOR

void run()
{
    int min;
    int *src,*dst, *temp;

    printf("NUMBER OF RUNS: %d\n",NUM_RUNS);
    long long start = get_time();

    for (int j=0; j<NUM_RUNS; j++) {
        src = new int[cols];
        for (int x = 0; x < cols; x++){
            result[x] = wall[x];
        }

        dst = result;
        for (int t = 0; t < rows-1; t++) {
            temp = src;
            src = dst;
            dst = temp;
            for(int n = 0; n < cols; n++){
              min = src[n];
              if (n > 0)
                min = MIN(min, src[n-1]);
              if (n < cols-1)
                min = MIN(min, src[n+1]);
              dst[n] = wall[(t+1)*cols + n]+min;
            }
        }
    }

    if(compare(cols, dst, reference)){
        printf("Verification failed!\n");
    } else {
        printf("Verification passed!\n");
    }

#ifdef RESULT_PRINT
    output_print(dst, cols);
#endif  // RESULT_PRINT

}

#else // USE_RISCV_VECTOR

void run_vector()
{
    int *dst;

    printf("NUMBER OF RUNS: %d L=%d C=%d\n",NUM_RUNS, NR_LANES, NR_CLUSTERS);
    
    start_timer();

#ifdef INTRINSICS
    for (int j=0; j<NUM_RUNS; j++) {
        for (int x = 0; x < cols; x++){
            result[x] = wall[x];
        }
        dst = result;

        size_t gvl = __riscv_vsetvl_e32m1(cols);

        _MMR_i32    xSrc_slideup;
        _MMR_i32    xSrc_slidedown;
        _MMR_i32    xSrc;
        _MMR_i32    xNextrow;

        int aux,aux2;

        for (size_t t = 0; t < rows-1; t++)
        {
            aux = dst[0] ;
            for(size_t n = 0; n < cols; n = n + gvl)
            {
                gvl = __riscv_vsetvl_e32m1(cols-n);
                xNextrow = _MM_LOAD_i32(&dst[n],gvl);
                xSrc = xNextrow;
                aux2 = (n+gvl >= cols) ?  dst[n+gvl-1] : dst[n+gvl];
                xSrc_slideup = _MM_VSLIDE1UP_i32(xSrc,aux,gvl);
                xSrc_slidedown = _MM_VSLIDE1DOWN_i32(xSrc,aux2,gvl);
                xSrc = _MM_MIN_i32(xSrc,xSrc_slideup,gvl);
                xSrc = _MM_MIN_i32(xSrc,xSrc_slidedown,gvl);
                xNextrow = _MM_LOAD_i32(&wall[(t+1)*cols + n],gvl);
                xNextrow = _MM_ADD_i32(xNextrow,xSrc,gvl);
                aux = dst[n+gvl-1];
                _MM_STORE_i32(&dst[n],xNextrow,gvl);
            }
        }
    }
#else
    for (int j=0; j<NUM_RUNS; j++) {
        for (int x = 0; x < cols; x++){
            result[x] = wall[x];
        }
        dst = result;

        size_t gvl;

        int aux, aux2;

        for (size_t t = 0; t < rows-1; t++)
        {
            aux = dst[0]; 
            for(size_t n = 0; n < cols; n = n + gvl)
            {
                asm volatile ("vsetvli %0, %1, e32, m8, ta, ma" : "=r"(gvl) : "r"(cols-n));
                if (!((t>0) && (gvl==cols)))
                    asm volatile ("vle32.v v0, (%0)"::"r"(&dst[n]));
                aux2 = (n+gvl >= cols) ?  dst[n+gvl-1] : dst[n+gvl];
                asm volatile ("vle32.v v24, (%0)"::"r"(&wall[(t+1)*cols + n]));
                asm volatile ("vslide1up.vx v16, v0, %0"::"r"(aux));
                asm volatile ("vmin.vv v0, v0, v16");
                asm volatile ("vslide1down.vx v8, v0, %0"::"r"(aux2));
                asm volatile ("vmin.vv v0, v0, v8");
                asm volatile ("vadd.vv v0, v0, v24");
                aux = dst[n+gvl-1];
                asm volatile ("vse32.v v0, (%0)"::"r"(&dst[n]));
            }
        }
    }
#endif // INTRINSICS
    stop_timer();

    if(compare(cols, dst, reference)){
        printf("Verification failed!\n");
    } else {
        int64_t cycles = get_timer();
        printf("Verification passed! [sw-cycles]=%ld\n", cycles);
    }

#ifdef RESULT_PRINT
    output_print(dst, cols);
#endif // RESULT_PRINT

}
#endif // USE_RISCV_VECTOR

void output_print(int *dst, int cols) {

    for (int j = 0; j < cols; j++){
        printf("%d ",reference[j]) ;
    }
        printf("\n") ;
}

bool compare( int cols, int *result, int *reference){

    bool error = 0;
    for (int j = 0; j < cols; j++){
        if(result[j] != reference[j]){
            error = 1;
        }
        return error;
    }
    return error;
}
