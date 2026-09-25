/* SE-mode compatible replacement for the bare-metal riscv_util.h:
 * same timing helpers, but baremetal_malloc() calls the real libc
 * allocator instead of bump-allocating from a linker-provided
 * l2_alloc_base symbol that doesn't exist under gem5 SE mode. */

#include <time.h>
#include <sys/time.h>
#include <stdlib.h>

#ifndef RISCV_UTIL_H
#define RISCV_UTIL_H

static long long get_time() {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (tv.tv_sec * 1000000) + tv.tv_usec;
}

static float elapsed_time(long long start_time, long long end_time) {
        return (float) (end_time - start_time) / (1000 * 1000);
}

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

#define ALIGNMENT (NR_LANES * NR_CLUSTERS * 4)
#define ALIGN_UP(x, a)  (((x) + (a) - 1) & ~((a) - 1))

inline void * baremetal_malloc(int incr)
{
    return malloc((size_t)incr);
}

#endif // RISCV_UTIL_H
