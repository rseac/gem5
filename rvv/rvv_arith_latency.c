#include <riscv_vector.h>
#include <stdio.h>

#ifndef SEW
#define SEW 32 // Default to 32-bit elements
#endif

int
main()
{
    printf("Starting Arithmetic Latency Test (SEW=%d)...\n", SEW);

#if SEW == 16
    size_t vl = __riscv_vsetvlmax_e16m1();
    vfloat16m1_t v1 = __riscv_vfmv_v_f_f16m1((_Float16)1.0f, vl);
    vfloat16m1_t v2 = __riscv_vfmv_v_f_f16m1((_Float16)2.0f, vl);
#elif SEW == 32
    size_t vl = __riscv_vsetvlmax_e32m1();
    vfloat32m1_t v1 = __riscv_vfmv_v_f_f32m1(1.0f, vl);
    vfloat32m1_t v2 = __riscv_vfmv_v_f_f32m1(2.0f, vl);
#elif SEW == 64
    size_t vl = __riscv_vsetvlmax_e64m1();
    vfloat64m1_t v1 = __riscv_vfmv_v_f_f64m1(1.0, vl);
    vfloat64m1_t v2 = __riscv_vfmv_v_f_f64m1(2.0, vl);
#else
#error "Unsupported SEW (Use 16, 32, or 64)"
#endif

    // Run a long dependency chain of additions
    // The result of one add is the input to the next.
    // This exposes the pipeline latency vs SEW variations.

    const int ITERATIONS = 10000;

    for (int i = 0; i < ITERATIONS; i++) {
#if SEW == 16
        v1 = __riscv_vfadd_vv_f16m1(v1, v2, vl);
        v1 = __riscv_vfadd_vv_f16m1(v1, v2, vl);
        v1 = __riscv_vfadd_vv_f16m1(v1, v2, vl);
        v1 = __riscv_vfadd_vv_f16m1(v1, v2, vl);
        v1 = __riscv_vfadd_vv_f16m1(v1, v2, vl);
        v1 = __riscv_vfadd_vv_f16m1(v1, v2, vl);
        v1 = __riscv_vfadd_vv_f16m1(v1, v2, vl);
        v1 = __riscv_vfadd_vv_f16m1(v1, v2, vl);
        v1 = __riscv_vfadd_vv_f16m1(v1, v2, vl);
        v1 = __riscv_vfadd_vv_f16m1(v1, v2, vl);
#elif SEW == 32
        v1 = __riscv_vfadd_vv_f32m1(v1, v2, vl);
        v1 = __riscv_vfadd_vv_f32m1(v1, v2, vl);
        v1 = __riscv_vfadd_vv_f32m1(v1, v2, vl);
        v1 = __riscv_vfadd_vv_f32m1(v1, v2, vl);
        v1 = __riscv_vfadd_vv_f32m1(v1, v2, vl);
        v1 = __riscv_vfadd_vv_f32m1(v1, v2, vl);
        v1 = __riscv_vfadd_vv_f32m1(v1, v2, vl);
        v1 = __riscv_vfadd_vv_f32m1(v1, v2, vl);
        v1 = __riscv_vfadd_vv_f32m1(v1, v2, vl);
        v1 = __riscv_vfadd_vv_f32m1(v1, v2, vl);
#elif SEW == 64
        v1 = __riscv_vfadd_vv_f64m1(v1, v2, vl);
        v1 = __riscv_vfadd_vv_f64m1(v1, v2, vl);
        v1 = __riscv_vfadd_vv_f64m1(v1, v2, vl);
        v1 = __riscv_vfadd_vv_f64m1(v1, v2, vl);
        v1 = __riscv_vfadd_vv_f64m1(v1, v2, vl);
        v1 = __riscv_vfadd_vv_f64m1(v1, v2, vl);
        v1 = __riscv_vfadd_vv_f64m1(v1, v2, vl);
        v1 = __riscv_vfadd_vv_f64m1(v1, v2, vl);
        v1 = __riscv_vfadd_vv_f64m1(v1, v2, vl);
        v1 = __riscv_vfadd_vv_f64m1(v1, v2, vl);
#endif
    }

    // Prevent optimization
#if SEW == 16
    float result = (float)__riscv_vfmv_f_s_f16m1_f16(v1);
#elif SEW == 32
    float result = __riscv_vfmv_f_s_f32m1_f32(v1);
#elif SEW == 64
    double result = __riscv_vfmv_f_s_f64m1_f64(v1);
#endif

    printf("Done. Result: %f\n", (double)result);

    return 0;
}
