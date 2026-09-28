// Isolation test: does a SINGLE SEW switch (32->64) hang, or only
// repeated switching?
#include <stdint.h>
#include "printf.h"
#include "runtime.h"

int main() {
    HW_CNT_READY;
    asm volatile("vsetvli zero, %0, e32, m1, ta, ma" : : "r"(16));
    asm volatile("vfmul.vv v8, v8, v9");
    asm volatile("vsetvli zero, %0, e64, m1, ta, ma" : : "r"(16));
    asm volatile("vfmul.vv v8, v8, v9");
    printf("ALL_DONE\n");
    return 0;
}
