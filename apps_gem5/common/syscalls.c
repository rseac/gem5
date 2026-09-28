// Copyright 2022-2025 ETH Zurich and University of Bologna.
//
// SPDX-License-Identifier: Apache-2.0
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//    http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
//
// Newlib's generic riscv _sbrk/_gettimeofday stubs issue an `ecall`, which
// traps into M-mode. Since crt0.S installs no mtvec_handler, that trap falls
// through to _fail, which spins forever in _eoc: any app calling libc
// malloc() or gettimeofday()/time() hangs the simulation with no output.
// Overriding these syscalls here (linked ahead of libc) keeps them
// ecall-free, matching how _putchar in serial.c avoids the same problem
// for printf.

#include <stddef.h>
#include <sys/time.h>

extern char l2_alloc_base; // Set by the linker script

void *_sbrk(ptrdiff_t incr) {
  static char *heap_end = 0;
  char *prev;

  if (heap_end == 0)
    heap_end = &l2_alloc_base;

  prev = heap_end;
  heap_end += incr;
  return (void *)prev;
}

int _gettimeofday(struct timeval *tv, void *tz) {
  (void)tz;
  if (tv) {
    tv->tv_sec = 0;
    tv->tv_usec = 0;
  }
  return 0;
}
