#ifndef TSVC_COMMON_HDR
#define TSVC_COMMON_HDR

#define iterations 1
#define LEN_1D 15360
#define LEN_2D 128

//#define iterations 100000
//#define LEN_1D 32000
//#define LEN_2D 256

#include <stdint.h>
#include <stdio.h>
#include <inttypes.h>
#include <sys/time.h>

// Helper to read hardware cycle counter
static inline uint64_t read_cycles() {
    uint64_t val;
    __asm__ volatile ("rdcycle %0" : "=r" (val));
    return val;
}

#ifdef RDCYCLE
    #undef USE_M5OPS
    #define RDCYCLE_VAL 1
#else
    #define RDCYCLE_VAL 0
#endif

#ifdef USE_M5OPS
#include <gem5/m5ops.h>
#define ROI_BEGIN(fa) do { m5_reset_stats(0, 0); (fa)->c1 = read_cycles(); gettimeofday(&(fa)->t1, NULL); } while (0)
#define ROI_END(fa)   do { gettimeofday(&(fa)->t2, NULL); (fa)->c2 = read_cycles(); m5_dump_reset_stats(0, 0); \
                           printf("cycles: %" PRIu64 " [M5_OPS]\n", (fa)->c2 - (fa)->c1); } while (0)
#else
#define ROI_BEGIN(fa) do { (fa)->c1 = read_cycles(); gettimeofday(&(fa)->t1, NULL); \
                           if (RDCYCLE_VAL) printf("start cycles: %" PRIu64 " [RDCYCLE]\n", (fa)->c1); } while (0)
#define ROI_END(fa)   do { gettimeofday(&(fa)->t2, NULL); (fa)->c2 = read_cycles(); \
                           if (RDCYCLE_VAL) printf("end cycles: %" PRIu64 " [RDCYCLE]\n", (fa)->c2); \
                           if (RDCYCLE_VAL) printf("cycles: %" PRIu64 " [RDCYCLE]\n", (fa)->c2 - (fa)->c1); } while (0)
#endif

struct args_t {
    struct timeval t1;
    struct timeval t2;
    uint64_t c1;
    uint64_t c2;
    void * __restrict__ arg_info;
};

#if 0
typedef double real_t;
#define ABS fabs
#else
typedef float real_t;
#define ABS fabsf
#endif

int dummy(real_t[LEN_1D], real_t[LEN_1D], real_t[LEN_1D], real_t[LEN_1D], real_t[LEN_1D], real_t[LEN_2D][LEN_2D], real_t[LEN_2D][LEN_2D], real_t[LEN_2D][LEN_2D], real_t);

void init(int** ip, real_t* s1, real_t* s2);

int initialise_arrays(const char* name);
real_t calc_checksum(const char * name);

#endif
