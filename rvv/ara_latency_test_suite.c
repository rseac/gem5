#include <riscv_vector.h>
#include <stdint.h>

#ifdef SPIKE
#include "util.h"
#include <stdio.h>
#elif defined ARA_LINUX
#include <stdio.h>
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
// This determines if the overhead is fixed or scales with vector occupancy.
void diag_vl_sweep() {
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
// This determines if the scalar issue rate is the bottleneck.
void diag_scalar_interleave() {
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
    printf("Vector+Scalar is %.1f%% of Pure Vector time.\n", (double)cycles_inter * 100.0 / cycles_pure);
}

// --- Standard Tests ---
#define TEST_LAT_2(NAME, SEW, TYPE, INIT, FUNC) \
void lat_##NAME() { \
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

#define INIT_INT8   __riscv_vmv_v_x_i8m1(1, __riscv_vsetvl_e8m1(1))
TEST_LAT_2(vadd_e8,   8,  vint8m1_t,  INIT_INT8,   __riscv_vadd_vv_i8m1)

int main() {
    HW_CNT_READY;
    enable_vector();
    printf("==================================================\n");
    printf("   ARA HARDWARE LATENCY DIAGNOSTIC SUITE\n");
    printf("   VLEN: %d bits | Lanes: 4\n", VLEN);
    printf("==================================================\n");

    lat_vadd_e8();
    diag_vl_sweep();
    diag_scalar_interleave();

    printf("\n==================================================\n");
    return 0;
}
