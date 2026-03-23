#include <riscv_vector.h>
#include <stdio.h>
#include <stdint.h>

// --- Configuration ---
#ifndef VLEN
#define VLEN 128
#endif

#define ITERATIONS 500
#define UNROLL 10

// --- Measurement Infrastructure ---
static inline uint64_t read_mcycle() {
    uint64_t val;
    asm volatile ("csrr %0, mcycle" : "=r" (val));
    return val;
}

// --- Test Macros ---

// Meaures Pipeline Latency (Unary - 1 operand)
#define TEST_LATENCY_1(NAME, SEW, TYPE, INIT_VAL, INST_FUNC) \
void test_latency_##NAME() { \
    size_t vl = __riscv_vsetvl_e##SEW##m1(1); \
    TYPE v1 = INIT_VAL; \
    uint64_t start, end; \
    start = read_mcycle(); \
    for (int i = 0; i < ITERATIONS; i++) { \
        v1 = INST_FUNC(v1, vl); v1 = INST_FUNC(v1, vl); \
        v1 = INST_FUNC(v1, vl); v1 = INST_FUNC(v1, vl); \
        v1 = INST_FUNC(v1, vl); v1 = INST_FUNC(v1, vl); \
        v1 = INST_FUNC(v1, vl); v1 = INST_FUNC(v1, vl); \
        v1 = INST_FUNC(v1, vl); v1 = INST_FUNC(v1, vl); \
    } \
    end = read_mcycle(); \
    double avg = (double)(end - start) / (ITERATIONS * UNROLL); \
    printf("LATENCY  [%-12s] SEW=%-2d: %6.2f cycles/inst\n", #NAME, SEW, avg); \
}

// Meaures Pipeline Latency (Binary - 2 operands)
#define TEST_LATENCY_2(NAME, SEW, TYPE, INIT_VAL, INST_FUNC) \
void test_latency_##NAME() { \
    size_t vl = __riscv_vsetvl_e##SEW##m1(1); \
    TYPE v1 = INIT_VAL; \
    TYPE v2 = INIT_VAL; \
    uint64_t start, end; \
    start = read_mcycle(); \
    for (int i = 0; i < ITERATIONS; i++) { \
        v1 = INST_FUNC(v1, v2, vl); v1 = INST_FUNC(v1, v2, vl); \
        v1 = INST_FUNC(v1, v2, vl); v1 = INST_FUNC(v1, v2, vl); \
        v1 = INST_FUNC(v1, v2, vl); v1 = INST_FUNC(v1, v2, vl); \
        v1 = INST_FUNC(v1, v2, vl); v1 = INST_FUNC(v1, v2, vl); \
        v1 = INST_FUNC(v1, v2, vl); v1 = INST_FUNC(v1, v2, vl); \
    } \
    end = read_mcycle(); \
    double avg = (double)(end - start) / (ITERATIONS * UNROLL); \
    printf("LATENCY  [%-12s] SEW=%-2d: %6.2f cycles/inst\n", #NAME, SEW, avg); \
}

// Measures Throughput/Occupancy (Binary - 2 operands independent)
#define TEST_THROUGHPUT_2(NAME, SEW, TYPE, INIT_VAL, INST_FUNC) \
void test_throughput_##NAME() { \
    size_t vl = __riscv_vsetvl_e##SEW##m1(VLEN/SEW); \
    TYPE v1 = INIT_VAL; TYPE v2 = INIT_VAL; \
    TYPE v3 = INIT_VAL; TYPE v4 = INIT_VAL; \
    uint64_t start, end; \
    start = read_mcycle(); \
    for (int i = 0; i < ITERATIONS; i++) { \
        v1 = INST_FUNC(v1, v2, vl); v3 = INST_FUNC(v3, v4, vl); \
        v1 = INST_FUNC(v1, v2, vl); v3 = INST_FUNC(v3, v4, vl); \
        v1 = INST_FUNC(v1, v2, vl); v3 = INST_FUNC(v3, v4, vl); \
        v1 = INST_FUNC(v1, v2, vl); v3 = INST_FUNC(v3, v4, vl); \
        v1 = INST_FUNC(v1, v2, vl); v3 = INST_FUNC(v3, v4, vl); \
    } \
    end = read_mcycle(); \
    double avg = (double)(end - start) / (ITERATIONS * UNROLL); \
    printf("THROUGH  [%-12s] SEW=%-2d: %6.2f cycles/inst\n", #NAME, SEW, avg); \
}

// Special case for slides
void test_latency_vslide_e32() {
    size_t vl = __riscv_vsetvl_e32m1(1);
    vint32m1_t v1 = __riscv_vundefined_i32m1();
    uint64_t start, end;
    start = read_mcycle();
    for (int i = 0; i < ITERATIONS; i++) {
        v1 = __riscv_vslidedown_vx_i32m1(v1, 1, vl); v1 = __riscv_vslidedown_vx_i32m1(v1, 1, vl);
        v1 = __riscv_vslidedown_vx_i32m1(v1, 1, vl); v1 = __riscv_vslidedown_vx_i32m1(v1, 1, vl);
        v1 = __riscv_vslidedown_vx_i32m1(v1, 1, vl); v1 = __riscv_vslidedown_vx_i32m1(v1, 1, vl);
        v1 = __riscv_vslidedown_vx_i32m1(v1, 1, vl); v1 = __riscv_vslidedown_vx_i32m1(v1, 1, vl);
        v1 = __riscv_vslidedown_vx_i32m1(v1, 1, vl); v1 = __riscv_vslidedown_vx_i32m1(v1, 1, vl);
    }
    end = read_mcycle();
    double avg = (double)(end - start) / (ITERATIONS * UNROLL);
    printf("LATENCY  [vslide_e32   ] SEW=32: %6.2f cycles/inst\n", avg);
}

// --- Integer Arithmetic (VFU_Alu) ---
#define INIT_INT(SEW) __riscv_vundefined_i##SEW##m1()
TEST_LATENCY_2(vadd_e8,   8,  vint8m1_t,  INIT_INT(8),   __riscv_vadd_vv_i8m1)
TEST_LATENCY_2(vadd_e16,  16, vint16m1_t, INIT_INT(16),  __riscv_vadd_vv_i16m1)
TEST_LATENCY_2(vadd_e32,  32, vint32m1_t, INIT_INT(32),  __riscv_vadd_vv_i32m1)
TEST_LATENCY_2(vadd_e64,  64, vint64m1_t, INIT_INT(64),  __riscv_vadd_vv_i64m1)

// --- Integer Multiply (VFU_Mul) ---
TEST_LATENCY_2(vmul_e8,   8,  vint8m1_t,  INIT_INT(8),   __riscv_vmul_vv_i8m1)
TEST_LATENCY_2(vmul_e32,  32, vint32m1_t, INIT_INT(32),  __riscv_vmul_vv_i32m1)
TEST_LATENCY_2(vmul_e64,  64, vint64m1_t, INIT_INT(64),  __riscv_vmul_vv_i64m1)

// --- Integer Divide (VFU_Div) ---
TEST_LATENCY_2(vdiv_e32,  32, vint32m1_t, INIT_INT(32),  __riscv_vdiv_vv_i32m1)
TEST_LATENCY_2(vdiv_e64,  64, vint64m1_t, INIT_INT(64),  __riscv_vdiv_vv_i64m1)

// --- Floating Point (VFU_MFpu) ---
#define INIT_FP32 __riscv_vfmv_v_f_f32m1(1.0f, __riscv_vsetvl_e32m1(1))
#define INIT_FP64 __riscv_vfmv_v_f_f64m1(1.0,  __riscv_vsetvl_e64m1(1))

TEST_LATENCY_2(vfadd_e32, 32, vfloat32m1_t, INIT_FP32, __riscv_vfadd_vv_f32m1)
TEST_LATENCY_2(vfadd_e64, 64, vfloat64m1_t, INIT_FP64, __riscv_vfadd_vv_f64m1)
TEST_LATENCY_2(vfmul_e32, 32, vfloat32m1_t, INIT_FP32, __riscv_vfmul_vv_f32m1)
TEST_LATENCY_2(vfmul_e64, 64, vfloat64m1_t, INIT_FP64, __riscv_vfmul_vv_f64m1)

// FP Div/Sqrt (Iterative)
TEST_LATENCY_2(vfdiv_e32, 32, vfloat32m1_t, INIT_FP32, __riscv_vfdiv_vv_f32m1)
TEST_LATENCY_1(vfsqrt_e32,32, vfloat32m1_t, INIT_FP32, __riscv_vfsqrt_v_f32m1)

// --- Category 6: Throughput (Lane Verification) ---
TEST_THROUGHPUT_2(vadd_thru_e32, 32, vint32m1_t, INIT_INT(32), __riscv_vadd_vv_i32m1)
TEST_THROUGHPUT_2(vadd_thru_e64, 64, vint64m1_t, INIT_INT(64), __riscv_vadd_vv_i64m1)

int main() {
    printf("==================================================\n");
    printf("   ARA HARDWARE LATENCY & LANE TEST SUITE\n");
    printf("   VLEN: %d bits\n", VLEN);
    printf("==================================================\n");
    fflush(stdout);

    printf("\n[1] Pipeline Latency (Dependent Chain)...\n");
    printf("Target: Pipeline Depth + Chaining Overhead\n");
    fflush(stdout);
    test_latency_vadd_e32();
    test_latency_vfadd_e32();
    test_latency_vfadd_e64();

    printf("\n[2] Throughput / Occupancy (Independent Stream)...\n");
    printf("Note: Cycles/Inst should match ceil(VLEN / (Lanes * SEW))\n");
    fflush(stdout);
    test_throughput_vadd_thru_e32();
    test_throughput_vadd_thru_e64();

    printf("\n[3] Integer Category Latencies...\n");
    fflush(stdout);
    test_latency_vadd_e8();
    test_latency_vadd_e16();
    test_latency_vadd_e64();
    test_latency_vmul_e8();
    test_latency_vmul_e32();
    test_latency_vmul_e64();
    test_latency_vdiv_e32();
    test_latency_vdiv_e64();

    printf("\n[4] Floating Point Category Latencies...\n");
    fflush(stdout);
    test_latency_vfmul_e32();
    test_latency_vfdiv_e32();
    test_latency_vfsqrt_e32();

    printf("\n[5] Misc Operations...\n");
    fflush(stdout);
    test_latency_vslide_e32();

    printf("\n==================================================\n");
    printf("   VERIFICATION COMPLETE\n");
    printf("==================================================\n");
    fflush(stdout);

    return 0;
}
