#include <riscv_vector.h>
#include <stdint.h>

static inline uint64_t read_cycles() {
    uint64_t val;
    asm volatile ("rdcycle %0" : "=r" (val));
    return val;
}

#ifdef SPIKE
#include "util.h"
#include <stdio.h>
#elif defined ARA_LINUX
#include <stdio.h>
#elif defined GEM5
#include <stdio.h>
// Use simple assignment to avoid redefinition errors in functions with multiple timers
#define start_timer() _s = read_cycles()
#define stop_timer()  _e = read_cycles()
#define get_timer()   (_e - _s)
#define HW_CNT_READY  uint64_t _s, _e
#else
#include "runtime.h"
#include "printf.h"
#endif

#ifndef VLEN
#define VLEN 4096
#endif

#define ITERATIONS 500
#define UNROLL 10

static void enable_vector() {
    asm volatile ("vsetvli zero, zero, e32, m1, ta, ma");
}

static void sink_result(void* ptr) {
    asm volatile("" : : "r"(ptr) : "memory");
}

// --- Diagnostic 1: VL Sweep ---
void diag_vl_sweep() {
    HW_CNT_READY;
    printf("\n[DIAG] Vector Length (VL) Sweep - SEW=32\n");
    printf("Lanes: 4 (Occupancy = ceil(VL/4))\n");
    printf("------------------------------------------\n");
    printf("VL   | Cycles/Inst | Occupancy | Overhead\n");
    printf("------------------------------------------\n");

    int vls[] = {1, 2, 4, 8, 16, 32, 64};
    for (int i = 0; i < 7; i++) {
        int target_vl = vls[i];
        size_t vl = __riscv_vsetvl_e32m1(target_vl);
        vint32m1_t v1 = __riscv_vmv_v_x_i32m1(1, vl);
        vint32m1_t v2 = __riscv_vmv_v_x_i32m1(1, vl);
        
        start_timer();
        for (int j = 0; j < ITERATIONS; j++) {
            v1 = __riscv_vadd_vv_i32m1(v1, v2, vl); v1 = __riscv_vadd_vv_i32m1(v1, v2, vl);
            v1 = __riscv_vadd_vv_i32m1(v1, v2, vl); v1 = __riscv_vadd_vv_i32m1(v1, v2, vl);
            v1 = __riscv_vadd_vv_i32m1(v1, v2, vl); v1 = __riscv_vadd_vv_i32m1(v1, v2, vl);
            v1 = __riscv_vadd_vv_i32m1(v1, v2, vl); v1 = __riscv_vadd_vv_i32m1(v1, v2, vl);
            v1 = __riscv_vadd_vv_i32m1(v1, v2, vl); v1 = __riscv_vadd_vv_i32m1(v1, v2, vl);
        }
        stop_timer();
        uint64_t cycles = get_timer();
        sink_result(&v1);
        
        double avg = (double)cycles / (ITERATIONS * UNROLL);
        int occ = (target_vl + 3) / 4; // ceil(vl/4)
        int pipe = 1;
        double overhead = avg - pipe - occ;
        
        printf("%-4d | %11.2f | %-9d | %8.2f\n", target_vl, avg, occ, overhead);
    }
}

// --- Diagnostic 2: Scalar Interleaving ---
void diag_scalar_interleave() {
    HW_CNT_READY;
    printf("\n[DIAG] Scalar Interleaving (Testing Issue Bottleneck)\n");
    size_t vl = __riscv_vsetvl_e32m1(1);
    vint32m1_t v1 = __riscv_vmv_v_x_i32m1(1, vl);
    vint32m1_t v2 = __riscv_vmv_v_x_i32m1(1, vl);
    volatile uint64_t s1 = 0;

    // Test 1: Pure Vector Chain
    start_timer();
    for (int i = 0; i < ITERATIONS; i++) {
        v1 = __riscv_vadd_vv_i32m1(v1, v2, vl); v1 = __riscv_vadd_vv_i32m1(v1, v2, vl);
        v1 = __riscv_vadd_vv_i32m1(v1, v2, vl); v1 = __riscv_vadd_vv_i32m1(v1, v2, vl);
        v1 = __riscv_vadd_vv_i32m1(v1, v2, vl); v1 = __riscv_vadd_vv_i32m1(v1, v2, vl);
        v1 = __riscv_vadd_vv_i32m1(v1, v2, vl); v1 = __riscv_vadd_vv_i32m1(v1, v2, vl);
        v1 = __riscv_vadd_vv_i32m1(v1, v2, vl); v1 = __riscv_vadd_vv_i32m1(v1, v2, vl);
    }
    stop_timer();
    uint64_t cycles_pure = get_timer();

    // Test 2: Interleaved (Vector + Scalar)
    start_timer();
    for (int i = 0; i < ITERATIONS; i++) {
        v1 = __riscv_vadd_vv_i32m1(v1, v2, vl); s1++;
        v1 = __riscv_vadd_vv_i32m1(v1, v2, vl); s1++;
        v1 = __riscv_vadd_vv_i32m1(v1, v2, vl); s1++;
        v1 = __riscv_vadd_vv_i32m1(v1, v2, vl); s1++;
        v1 = __riscv_vadd_vv_i32m1(v1, v2, vl); s1++;
        v1 = __riscv_vadd_vv_i32m1(v1, v2, vl); s1++;
        v1 = __riscv_vadd_vv_i32m1(v1, v2, vl); s1++;
        v1 = __riscv_vadd_vv_i32m1(v1, v2, vl); s1++;
        v1 = __riscv_vadd_vv_i32m1(v1, v2, vl); s1++;
        v1 = __riscv_vadd_vv_i32m1(v1, v2, vl); s1++;
    }
    stop_timer();
    uint64_t cycles_inter = get_timer();
    sink_result(&v1);
    sink_result((void*)&s1);

    printf("Pure Vector: %llu cycles, Interleaved: %llu cycles\n", cycles_pure, cycles_inter);
}

#define TEST_LAT_2(NAME, SEW, TYPE, INIT, FUNC) \
void lat_##NAME() { \
    HW_CNT_READY; \
    printf("RUNNING_%s\n", #NAME); \
    size_t vl = __riscv_vsetvl_e##SEW##m1(1); \
    TYPE v1 = INIT; TYPE v2 = INIT; \
    start_timer(); \
    for (int i = 0; i < ITERATIONS; i++) { \
        v1 = FUNC(v1, v2, vl); v1 = FUNC(v1, v2, vl); v1 = FUNC(v1, v2, vl); v1 = FUNC(v1, v2, vl); v1 = FUNC(v1, v2, vl); \
        v1 = FUNC(v1, v2, vl); v1 = FUNC(v1, v2, vl); v1 = FUNC(v1, v2, vl); v1 = FUNC(v1, v2, vl); v1 = FUNC(v1, v2, vl); \
    } \
    stop_timer(); \
    uint64_t cycles = get_timer(); \
    sink_result(&v1); \
    printf("DATA_POINT %s %d %llu %d\n", #NAME, SEW, cycles, (ITERATIONS * UNROLL)); \
}

#define TEST_THROUGH_2(NAME, SEW, TYPE, INIT, FUNC) \
void thru_##NAME() { \
    HW_CNT_READY; \
    printf("RUNNING_THROUGH_%s\n", #NAME); \
    size_t vl = __riscv_vsetvl_e##SEW##m1(VLEN/SEW); \
    TYPE v1 = INIT; TYPE v2 = INIT; TYPE v3 = INIT; TYPE v4 = INIT; \
    start_timer(); \
    for (int i = 0; i < ITERATIONS; i++) { \
        v1 = FUNC(v1, v2, vl); v3 = FUNC(v3, v4, vl); v1 = FUNC(v1, v2, vl); v3 = FUNC(v3, v4, vl); v1 = FUNC(v1, v2, vl); \
        v3 = FUNC(v3, v4, vl); v1 = FUNC(v1, v2, vl); v3 = FUNC(v3, v4, vl); v1 = FUNC(v1, v2, vl); v3 = FUNC(v3, v4, vl); \
    } \
    stop_timer(); \
    uint64_t cycles = get_timer(); \
    sink_result(&v1); sink_result(&v3); \
    printf("DATA_THROUGH %s %d %llu %d\n", #NAME, SEW, cycles, (ITERATIONS * UNROLL)); \
}

#define INIT_INT32  __riscv_vmv_v_x_i32m1(1, __riscv_vsetvl_e32m1(1))
#define INIT_INT64  __riscv_vmv_v_x_i64m1(1, __riscv_vsetvl_e64m1(1))

TEST_LAT_2(vadd_e32, 32, vint32m1_t, INIT_INT32, __riscv_vadd_vv_i32m1)
TEST_LAT_2(vadd_e64, 64, vint64m1_t, INIT_INT64, __riscv_vadd_vv_i64m1)
TEST_THROUGH_2(vadd_thru_e32, 32, vint32m1_t, INIT_INT32, __riscv_vadd_vv_i32m1)

int main() {
    enable_vector();
    printf("==================================================\n");
    printf("   ARA HARDWARE LATENCY & LANE TEST SUITE\n");
    printf("   VLEN: %d bits | Lanes: 4\n", VLEN);
    printf("==================================================\n");

    lat_vadd_e32();
    lat_vadd_e64();
    thru_vadd_thru_e32();
    diag_vl_sweep();
    diag_scalar_interleave();

    printf("\n==================================================\n");
    return 0;
}
