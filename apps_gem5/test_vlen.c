#include <stdint.h>
#include "printf.h"
#include "runtime.h"

int main() {
    uint64_t vlenb, vl;
    asm volatile("csrr %0, vlenb" : "=r"(vlenb));
    asm volatile("vsetvli %0, x0, e64, m1, ta, ma" : "=r"(vl));
    printf("HARDWARE_PROBE: VLEN=%lu bits (VLENB=%lu bytes), e64m1_max_vl=%lu\n", vlenb * 8, vlenb, vl);
    return 0;
}
