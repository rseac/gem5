#include <stdint.h>

/* Globals declared extern in apps_gem5/common/runtime.h that the
 * bare-metal crt0/linker normally provides; under SE mode we just
 * define them ourselves. */
int64_t event_trigger = 0;
int64_t timer = 0;
uint64_t hw_cnt_en_reg = 0;
void _putchar(char character) { __builtin_putchar(character); }
