#include <stdio.h>
#if __riscv_v >= 1000000
#include <riscv_vector.h>
#endif /* __riscv_v_intrinsic */

size_t a[8] = {1,2,3,4,5,6,7,8};
size_t b[8] = {4,5,6,7,8,9,10,11};
size_t e[4] = {};

int main() {

    size_t vl = __riscv_vsetvl_e64m1(4);
    vint64m1_t c = __riscv_vlse64_v_i64m1(&a[1], 16, vl);
    vint64m1_t d = __riscv_vlse64_v_i64m1(b, 16, vl);
    __riscv_vse64_v_i64m1(e, __riscv_vadd(c, d, vl), vl);

    return 0;
}
