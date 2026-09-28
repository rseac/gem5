/*************************************************************************
*GET_TIME
*returns a long int representing the time
*************************************************************************/

#include <time.h>
#include <sys/time.h>
#include <stdint.h>
#define NR_LANES 4
#define NR_CLUSTERS 1


#ifndef RISCV_UTIL_H
#define RISCV_UTIL_H

static long long get_time() {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (tv.tv_sec * 1000000) + tv.tv_usec;
}

// Returns the number of seconds elapsed between the two specified times
static float elapsed_time(long long start_time, long long end_time) {
        return (float) (end_time - start_time) / (1000 * 1000);
}

/*************************************************************************
*Instruction Count
*returns the number of instructions executed
*************************************************************************/
static unsigned long get_inst_count()
{
    unsigned long instr;
    asm volatile ("rdinstret %[instr]"
                : [instr]"=r"(instr));
    return instr;
}

static unsigned long get_cycles_count()
{
    unsigned long cycles;
    asm volatile ("rdcycle %[cycles]"
                : [cycles]"=r"(cycles));
    return cycles;
}

/*************************************************************************/

#define ALIGNMENT (NR_LANES * NR_CLUSTERS * 4)

#define ALIGN_UP(x, a)  (((x) + (a) - 1) & ~((a) - 1))

inline void * baremetal_malloc(int incr)
{
    extern char l2_alloc_base;   /* Set by linker */
    static char *heap_end = 0;

    uintptr_t aligned;
    char *result;

    /* First call: initialize heap */
    if (!heap_end)
        heap_end = &l2_alloc_base;

    /* Align current heap pointer */
    aligned = ALIGN_UP((uintptr_t)heap_end, ALIGNMENT);

    result = (char *)aligned;

    /* Move heap past allocated block */
    heap_end = result + incr;

    return (void *)result;
}

#endif // RISCV_UTIL_H