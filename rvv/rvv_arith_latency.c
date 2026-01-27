#include <riscv_vector.h>
#include <stdio.h>

int
main()
{
    printf("Starting Arithmetic Latency Test...\n");

    // Set VLMAX for e32, m1
    size_t vl = __riscv_vsetvlmax_e32m1();

    // Initialize vectors
    vfloat32m1_t v1 = __riscv_vfmv_v_f_f32m1(1.0f, vl);
    vfloat32m1_t v2 = __riscv_vfmv_v_f_f32m1(2.0f, vl);

    // Run a long dependency chain of additions
    // v1 = v1 + v2
    // The result of one add is the input to the next.
    // This exposes the pipeline latency.

    // Unrolling slightly to minimize loop overhead, but keeping dependency
    const int ITERATIONS = 10000;

    for (int i = 0; i < ITERATIONS; i++) {
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
    }

    // Prevent optimization
    float result = __riscv_vfmv_f_s_f32m1_f32(v1);
    printf("Done. Result: %f\n", result);

    return 0;
}
