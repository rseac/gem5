#ifndef TSVC_COMMON_HDR
#define TSVC_COMMON_HDR

#define iterations 1
#define LEN_1D 32000
#define LEN_2D 256

//#define iterations 100000
//#define LEN_1D 32000
//#define LEN_2D 256

#include <stdint.h>
#include <sys/time.h>

static inline uint64_t read_cycles(void)
{
    uint64_t c;
    __asm__ volatile ("rdcycle %0" : "=r"(c));
    return c;
}

#ifdef USE_M5OPS
#include <gem5/m5ops.h>
#define ROI_BEGIN(fa) do { m5_reset_stats(0, 0); (fa)->c1 = read_cycles(); gettimeofday(&(fa)->t1, NULL); } while (0)
#define ROI_END(fa)   do { gettimeofday(&(fa)->t2, NULL); (fa)->c2 = read_cycles(); m5_dump_reset_stats(0, 0); } while (0)
#else
#define ROI_BEGIN(fa) do { (fa)->c1 = read_cycles(); gettimeofday(&(fa)->t1, NULL); } while (0)
#define ROI_END(fa)   do { gettimeofday(&(fa)->t2, NULL); (fa)->c2 = read_cycles(); } while (0)
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
