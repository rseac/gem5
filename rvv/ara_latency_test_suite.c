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

// Increased iterations for better accuracy on large VLEN
#define ITERATIONS 500
#define UNROLL 10

static void enable_vector() {
    asm volatile ("vsetvli zero, zero, e32, m1, ta, ma");
}

// Strong optimization sink: prevents the compiler from deleting the vector loops
// by forcing it to assume the vector variable's memory is read.
static void sink_result(void* ptr) {
    asm volatile("" : : "r"(ptr) : "memory");
}

#define TEST_LAT_1(NAME, SEW, TYPE, INIT, FUNC) \
void lat_##NAME() { \
    printf("RUNNING_%s\n", #NAME); \
    size_t vl = __riscv_vsetvl_e##SEW##m1(1); \
    TYPE v1 = INIT; \
    start_timer(); \
    for (int i = 0; i < ITERATIONS; i++) { \
        v1 = FUNC(v1, vl); v1 = FUNC(v1, vl); v1 = FUNC(v1, vl); v1 = FUNC(v1, vl); v1 = FUNC(v1, vl); \
        v1 = FUNC(v1, vl); v1 = FUNC(v1, vl); v1 = FUNC(v1, vl); v1 = FUNC(v1, vl); v1 = FUNC(v1, vl); \
    } \
    stop_timer(); \
    uint64_t cycles = get_timer(); \
    sink_result(&v1); \
    printf("DATA_POINT %s %d %llu %d\n", #NAME, SEW, cycles, (ITERATIONS * UNROLL)); \
}

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

#define TEST_THROUGH_2(NAME, SEW, TYPE, INIT, FUNC) \
void thru_##NAME() { \
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

// Special case for slides
void lat_vslide_e32() {
    printf("RUNNING_vslide_e32\n");
    size_t vl = __riscv_vsetvl_e32m1(1);
    vint32m1_t v1 = __riscv_vundefined_i32m1();
    start_timer();
    for (int i = 0; i < ITERATIONS; i++) {
        v1 = __riscv_vslidedown_vx_i32m1(v1, 1, vl); v1 = __riscv_vslidedown_vx_i32m1(v1, 1, vl);
        v1 = __riscv_vslidedown_vx_i32m1(v1, 1, vl); v1 = __riscv_vslidedown_vx_i32m1(v1, 1, vl);
        v1 = __riscv_vslidedown_vx_i32m1(v1, 1, vl); v1 = __riscv_vslidedown_vx_i32m1(v1, 1, vl);
        v1 = __riscv_vslidedown_vx_i32m1(v1, 1, vl); v1 = __riscv_vslidedown_vx_i32m1(v1, 1, vl);
        v1 = __riscv_vslidedown_vx_i32m1(v1, 1, vl); v1 = __riscv_vslidedown_vx_i32m1(v1, 1, vl);
    }
    stop_timer();
    uint64_t cycles = get_timer();
    sink_result(&v1);
    printf("DATA_POINT vslide_e32 32 %llu %d\n", cycles, (ITERATIONS * UNROLL));
}

#define INIT_INT(S) __riscv_vundefined_i##S##m1()
#define INIT_FP32 __riscv_vfmv_v_f_f32m1(1.0f, __riscv_vsetvl_e32m1(1))
#define INIT_FP64 __riscv_vfmv_v_f_f64m1(1.0,  __riscv_vsetvl_e64m1(1))

// VFU_Alu
TEST_LAT_2(vadd_e8,   8,  vint8m1_t,  INIT_INT(8),   __riscv_vadd_vv_i8m1)
TEST_LAT_2(vadd_e16,  16, vint16m1_t, INIT_INT(16),  __riscv_vadd_vv_i16m1)
TEST_LAT_2(vadd_e32,  32, vint32m1_t, INIT_INT(32),  __riscv_vadd_vv_i32m1)
TEST_LAT_2(vadd_e64,  64, vint64m1_t, INIT_INT(64),  __riscv_vadd_vv_i64m1)
TEST_LAT_2(vor_e32,   32, vint32m1_t, INIT_INT(32),  __riscv_vor_vv_i32m1)

// VFU_Mul
TEST_LAT_2(vmul_e8,   8,  vint8m1_t,  INIT_INT(8),   __riscv_vmul_vv_i8m1)
TEST_LAT_2(vmul_e32,  32, vint32m1_t, INIT_INT(32),  __riscv_vmul_vv_i32m1)
TEST_LAT_2(vmul_e64,  64, vint64m1_t, INIT_INT(64),  __riscv_vmul_vv_i64m1)

// VFU_Div
TEST_LAT_2(vdiv_e32,  32, vint32m1_t, INIT_INT(32),  __riscv_vdiv_vv_i32m1)
TEST_LAT_2(vdiv_e64,  64, vint64m1_t, INIT_INT(64),  __riscv_vdiv_vv_i64m1)

// VFU_MFpu
TEST_LAT_2(vfadd_e32, 32, vfloat32m1_t, INIT_FP32,    __riscv_vfadd_vv_f32m1)
TEST_LAT_2(vfadd_e64, 64, vfloat64m1_t, INIT_FP64,    __riscv_vfadd_vv_f64m1)
TEST_LAT_2(vfmul_e32, 32, vfloat32m1_t, INIT_FP32,    __riscv_vfmul_vv_f32m1)
TEST_LAT_2(vfdiv_e32, 32, vfloat32m1_t, INIT_FP32,    __riscv_vfdiv_vv_f32m1)
TEST_LAT_1(vfsqrt_e32,32, vfloat32m1_t, INIT_FP32,    __riscv_vfsqrt_v_f32m1)

// Throughput
TEST_THROUGH_2(vadd_thru_e32, 32, vint32m1_t, INIT_INT(32), __riscv_vadd_vv_i32m1)
TEST_THROUGH_2(vadd_thru_e64, 64, vint64m1_t, INIT_INT(64), __riscv_vadd_vv_i64m1)

int main() {
    HW_CNT_READY;
    enable_vector();
    printf("VLEN_CHECK: %d\n", VLEN);
    
    lat_vadd_e8();   lat_vadd_e16();  lat_vadd_e32();  lat_vadd_e64();
    lat_vor_e32();
    lat_vmul_e8();   lat_vmul_e32();  lat_vmul_e64();
    lat_vdiv_e32();  lat_vdiv_e64();
    lat_vfadd_e32(); lat_vfadd_e64(); lat_vfmul_e32();
    lat_vfdiv_e32(); lat_vfsqrt_e32();
    lat_vslide_e32();
    
    thru_vadd_thru_e32();
    thru_vadd_thru_e64();

    printf("ALL_FINISHED\n");
    return 0;
}
