#include <stdio.h>
#include <gem5/m5ops.h>
#if __riscv_v >= 1000000
#include <riscv_vector.h>
#endif /* __riscv_v_intrinsic */

size_t a[11] = {1,2,3,4,5,6,7,8,9,10,11};
size_t b[8] = {4,5,6,7,8,9,10,11};
size_t e[8] = {};

int main() {

    size_t vl = __riscv_vsetvl_e64m1(4);
    vint64m1_t c = __riscv_vlse64_v_i64m1(&a[1], 24, vl);
    vint64m1_t d = __riscv_vle64_v_i64m1(b, vl);
    __riscv_vsse64_v_i64m1(e, 16, __riscv_vadd(c, d, vl), vl);
    m5_write_file(e, sizeof(e), 0, "stride1_result.bin");

    return 0;
}
