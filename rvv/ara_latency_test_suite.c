#include <riscv_vector.h>
#include <stdio.h>
#include <stdint.h>

#ifndef VLEN
#define VLEN 128
#endif

#define ITERATIONS 100
#define UNROLL 10

static inline uint64_t read_cycles() {
    uint64_t val;
    // Use 'rdcycle' which is usually available in all modes
    asm volatile ("rdcycle %0" : "=r" (val));
    return val;
}

// Safer vector enable - some testbenches don't allow mstatus access
static void enable_vector() {
    printf("Enabling Vector Unit...\n");
#ifdef ENABLE_MSTATUS_VS
    unsigned long mstatus;
    asm volatile ("csrr %0, mstatus" : "=r" (mstatus));
    mstatus |= (3 << 9); // Set VS to Dirty
    asm volatile ("csrw mstatus, %0" : : "r" (mstatus));
#endif
    // Trigger a simple vector instruction to see if it's alive
    asm volatile ("vsetvli zero, zero, e32, m1, ta, ma");
    printf("Vector Unit Ready.\n");
}

#define TEST_LATENCY_2(NAME, SEW, TYPE, INIT_VAL, INST_FUNC) \
void test_latency_##NAME() { \
    printf("RUNNING_%s\n", #NAME); \
    size_t vl = __riscv_vsetvl_e##SEW##m1(1); \
    TYPE v1 = INIT_VAL; \
    TYPE v2 = INIT_VAL; \
    uint64_t start, end; \
    start = read_cycles(); \
    for (int i = 0; i < ITERATIONS; i++) { \
        v1 = INST_FUNC(v1, v2, vl); v1 = INST_FUNC(v1, v2, vl); \
        v1 = INST_FUNC(v1, v2, vl); v1 = INST_FUNC(v1, v2, vl); \
        v1 = INST_FUNC(v1, v2, vl); v1 = INST_FUNC(v1, v2, vl); \
        v1 = INST_FUNC(v1, v2, vl); v1 = INST_FUNC(v1, v2, vl); \
        v1 = INST_FUNC(v1, v2, vl); v1 = INST_FUNC(v1, v2, vl); \
    } \
    end = read_cycles(); \
    printf("DATA_POINT %s %d %llu %d\n", #NAME, SEW, (end - start), (ITERATIONS * UNROLL)); \
}

#define TEST_THROUGHPUT_2(NAME, SEW, TYPE, INIT_VAL, INST_FUNC) \
void test_throughput_##NAME() { \
    printf("RUNNING_THROUGH_%s\n", #NAME); \
    size_t vl = __riscv_vsetvl_e##SEW##m1(VLEN/SEW); \
    TYPE v1 = INIT_VAL; TYPE v2 = INIT_VAL; \
    TYPE v3 = INIT_VAL; TYPE v4 = INIT_VAL; \
    uint64_t start, end; \
    start = read_cycles(); \
    for (int i = 0; i < ITERATIONS; i++) { \
        v1 = INST_FUNC(v1, v2, vl); v3 = INST_FUNC(v3, v4, vl); \
        v1 = INST_FUNC(v1, v2, vl); v3 = INST_FUNC(v3, v4, vl); \
        v1 = INST_FUNC(v1, v2, vl); v3 = INST_FUNC(v3, v4, vl); \
        v1 = INST_FUNC(v1, v2, vl); v3 = INST_FUNC(v3, v4, vl); \
        v1 = INST_FUNC(v1, v2, vl); v3 = INST_FUNC(v3, v4, vl); \
    } \
    end = read_cycles(); \
    printf("DATA_THROUGH %s %d %llu %d\n", #NAME, SEW, (end - start), (ITERATIONS * UNROLL)); \
}

#define INIT_INT(SEW) __riscv_vundefined_i##SEW##m1()
TEST_LATENCY_2(vadd_e32, 32, vint32m1_t, INIT_INT(32), __riscv_vadd_vv_i32m1)
TEST_LATENCY_2(vadd_e64, 64, vint64m1_t, INIT_INT(64), __riscv_vadd_vv_i64m1)
TEST_THROUGHPUT_2(vadd_thru_e32, 32, vint32m1_t, INIT_INT(32), __riscv_vadd_vv_i32m1)

int main() {
    printf("VLEN_CHECK: %d\n", VLEN);
    enable_vector();
    
    test_latency_vadd_e32();
    test_latency_vadd_e64();
    test_throughput_vadd_thru_e32();

    printf("ALL_FINISHED\n");
    return 0;
}
