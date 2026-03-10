#include <riscv_vector.h>
#include <stdio.h>

int main() {
    const int LMUL = 8;
    const int ITERATIONS = 1000;
    
    printf("Starting Vector Chaining Test (LMUL=%d)...\n", LMUL);

    size_t vl = __riscv_vsetvlmax_e32m8();
    vfloat32m8_t v1 = __riscv_vfmv_v_f_f32m8(1.0f, vl);
    vfloat32m8_t v2 = __riscv_vfmv_v_f_f32m8(2.0f, vl);

    // This loop creates a dependency chain that benefits from chaining.
    // vfmul can start processing the first elements of v1 as soon as
    // vfadd has finished its first lane's elements.
    for (int i = 0; i < ITERATIONS; i++) {
        v1 = __riscv_vfadd_vv_f32m8(v1, v2, vl);
        v1 = __riscv_vfmul_vv_f32m8(v1, v2, vl);
    }

    float result = __riscv_vfmv_f_s_f32m8_f32(v1);
    printf("Done. Result: %f\n", (double)result);

    return 0;
}
