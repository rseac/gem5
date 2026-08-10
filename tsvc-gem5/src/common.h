#ifndef TSVC_COMMON_HDR
#define TSVC_COMMON_HDR

#if defined(TINY)
    #define LEN_1D 15360
    #define LEN_2D 128
#elif defined(SMALL)
    #define LEN_1D 30720
    #define LEN_2D 256
#elif defined(MEDIUM)
    #define LEN_1D 256000
    #define LEN_2D 512
#elif defined(LARGE)
    #define LEN_1D 1024000
    #define LEN_2D 1024
#elif defined(HUGE)
    #define LEN_1D 4096000
    #define LEN_2D 2048
#elif defined(SMALL)
    #define LEN_1D 150360
    #define LEN_2D 256
#else
    #ifndef LEN_1D
        #define LEN_1D 32000
    #endif
    #ifndef LEN_2D
        #define LEN_2D 256
    #endif
#endif

// iterations is now a runtime variable
extern int iterations_val;
#define iterations iterations_val

// Closed form of the stock ip[] initialisation in common.c: each aligned
// 5-block holds {i+4, i+2, i, i+3, i+1} — (4 + 3*r) % 5 = {4,2,0,3,1} for
// r = 0..4 — and the partial final block (when LEN_1D % 5 != 0) is the
// identity. Lets the non-indexed kernel rewrites reproduce an ip[] value
// with pure arithmetic on i, i.e. without any memory read of ip[]. Invalid
// if the randomized Fisher-Yates init in common.c is enabled.
#define IP_STOCK(i) ((i) < LEN_1D - LEN_1D % 5 \
                     ? (i) - (i) % 5 + (4 + 3 * ((i) % 5)) % 5 : (i))

#include <stdint.h>
#include <stdio.h>
#include <inttypes.h>
#include <sys/time.h>
#include <math.h>

#define ABS(x) ((x)<0 ? -(x) : (x))
#define MIN(a,b) ((a)<(b) ? (a) : (b))
#define MAX(a,b) ((a)>(b) ? (a) : (b))

#define ARRAY_ALIGNMENT 64

// Helper to read hardware cycle counter
static inline uint64_t read_cycles() {
    uint64_t val = 0;
#if defined(__riscv)
    __asm__ volatile ("rdcycle %0" : "=r" (val));
#endif
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
#define ROI_BEGIN(fa) do { m5_work_begin(0, 0); m5_reset_stats(0, 0); (fa)->c1 = read_cycles(); } while (0)
#define ROI_END(fa)   do { (fa)->c2 = read_cycles(); m5_dump_reset_stats(0, 0); m5_work_end(0, 0); } while (0)
#define ROI_PRINT(fa) do { printf("cycles: %" PRIu64 " [M5_OPS]\n", (fa)->c2 - (fa)->c1); } while (0)
#else
#define ROI_BEGIN(fa) do { (fa)->c1 = read_cycles(); } while (0)
#define ROI_END(fa)   do { (fa)->c2 = read_cycles(); } while (0)
#define ROI_PRINT(fa) do { \
    if (RDCYCLE_VAL) { \
        printf("start cycles: %" PRIu64 " [RDCYCLE]\n", (fa)->c1); \
        printf("end cycles: %" PRIu64 " [RDCYCLE]\n", (fa)->c2); \
        printf("cycles: %" PRIu64 " [RDCYCLE]\n", (fa)->c2 - (fa)->c1); \
    } \
} while (0)
#endif

struct args_t {
    struct timeval t1;
    struct timeval t2;
    uint64_t c1;
    uint64_t c2;
    void * arg_info;
};

typedef float real_t;

int dummy(real_t[LEN_1D], real_t[LEN_1D], real_t[LEN_1D], real_t[LEN_1D], real_t[LEN_1D], real_t[LEN_2D][LEN_2D], real_t[LEN_2D][LEN_2D], real_t[LEN_2D][LEN_2D], real_t);

void init(int** ip, real_t* s1, real_t* s2);

int initialise_arrays(const char* name);
real_t calc_checksum(const char * name);

#endif
