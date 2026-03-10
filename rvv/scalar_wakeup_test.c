#include <riscv_vector.h>
#include <stdio.h>

int main() {
    const int ITERATIONS = 1000;
    
    printf("Starting Scalar Wakeup Test...\n");

    size_t vl = __riscv_vsetvlmax_e32m1();
    vfloat32m1_t v1 = __riscv_vfmv_v_f_f32m1(1.0f, vl);
    vfloat32m1_t v_init = __riscv_vfmv_v_f_f32m1(0.0f, vl);

    float s_res = 0.0f;

    // Reduction is a "serializing" operation for the scalar result.
    // The scalar move must wait for the FULL vector operation to finish.
    for (int i = 0; i < ITERATIONS; i++) {
        vfloat32m1_t v_sum = __riscv_vfredusum_vs_f32m1_f32m1(v1, v_init, vl);
        s_res = __riscv_vfmv_f_s_f32m1_f32(v_sum);
        s_res += 1.0f;
        v1 = __riscv_vfmv_v_f_f32m1(s_res, vl);
    }

    printf("Done. Result: %f\n", (double)s_res);

    return 0;
}
