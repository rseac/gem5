/*
 * rvv_load_chain_test.c
 *
 * Tests vector load-to-compute chaining in the gem5 AraO3 model.
 *
 * Three test scenarios are measured:
 *
 *   1. vle32 -> vfmul   (load result used directly by multiply)
 *   2. vle32 -> vfmacc  (axpy-style: acc += loaded_a * b, L1-resident)
 *   3. vfmul baseline   (operands pre-loaded, no dependency on a load)
 *
 * With load chaining ENABLED the vle32->vfmul and vle32->vfmacc chains
 * should complete with only CHAINING_OVERHEAD=2 cycles of extra latency
 * beyond the compute unit's pipeline depth, rather than waiting for the
 * full load instruction to write back before the consumer can issue.
 *
 * With load chaining DISABLED the consumer must wait for the load's full
 * writeback, so the chained cases should be strictly slower (or equal for
 * very long vectors where the overhead is amortised).
 *
 * Correctness is verified by comparing computed output against a reference
 * computed in scalar C.
 *
 * Build:
 *   riscv64-unknown-linux-gnu-gcc -O3 -march=rv64gcv -mabi=lp64d \
 *       -static -o rvv_load_chain_test.bin rvv_load_chain_test.c -lm
 *
 * Run under gem5 (see run_load_chain_test.py):
 *   build/RISCV/gem5.opt riscv-rvv-se-ara.py \
 *       --enable-chaining rvv_load_chain_test.bin
 */

#include <riscv_vector.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---------- cycle counter ------------------------------------------------ */
static inline uint64_t
read_cycles(void)
{
    uint64_t v;
    asm volatile("rdcycle %0" : "=r"(v));
    return v;
}

/* ---------- parameters --------------------------------------------------- */
#define N          512      /* array length (fits comfortably in 32 KiB L1D) */
#define ITERATIONS  50      /* repetitions for stable measurement            */
#define WARMUP       4      /* iterations discarded to prime caches/pipeline  */

/* ---------- helpers ------------------------------------------------------- */
static void
sink(const void *p)
{
    asm volatile("" :: "r"(p) : "memory");
}

/* Scalar reference for vle32->vfmul: dst[i] = a[i] * b[i] */
static void
ref_vmul(const float *a, const float *b, float *dst, int n)
{
    for (int i = 0; i < n; i++)
        dst[i] = a[i] * b[i];
}

/* Scalar reference for vle32->vfmacc: acc[i] += a[i] * b[i], iters times */
static void
ref_vfmacc(const float *a, const float *b, float *acc, int n, int iters)
{
    for (int it = 0; it < iters; it++)
        for (int i = 0; i < n; i++)
            acc[i] += a[i] * b[i];
}

/* Check two float arrays element-wise; return 0 on mismatch */
static int
check_close(const float *got, const float *ref, int n, float rtol)
{
    for (int i = 0; i < n; i++) {
        float diff = fabsf(got[i] - ref[i]);
        float mag  = fabsf(ref[i]) + 1e-6f;
        if (diff / mag > rtol) {
            printf("  MISMATCH at [%d]: got=%.6f  ref=%.6f\n",
                   i, (double)got[i], (double)ref[i]);
            return 0;
        }
    }
    return 1;
}

/* ---------- test 1: vle32 -> vfmul --------------------------------------- */
static void
test_load_mul(const float *src_a, const float *src_b,
              float *dst, int n)
{
    printf("--- Test 1: vle32 -> vfmul (L1-resident, LMUL=m8) ---\n");

    size_t vl = __riscv_vsetvlmax_e32m8();
    if ((int)vl > n) vl = (size_t)n;

    /* Warmup */
    for (int i = 0; i < WARMUP; i++) {
        vfloat32m8_t va = __riscv_vle32_v_f32m8(src_a, vl);
        vfloat32m8_t vb = __riscv_vle32_v_f32m8(src_b, vl);
        vfloat32m8_t vc = __riscv_vfmul_vv_f32m8(va, vb, vl);
        __riscv_vse32_v_f32m8(dst, vc, vl);
        sink(dst);
    }

    uint64_t t0 = read_cycles();
    for (int it = 0; it < ITERATIONS; it++) {
        /* Chain: load a, immediately multiply with pre-loaded b */
        vfloat32m8_t va = __riscv_vle32_v_f32m8(src_a, vl);
        vfloat32m8_t vb = __riscv_vle32_v_f32m8(src_b, vl);
        vfloat32m8_t vc = __riscv_vfmul_vv_f32m8(va, vb, vl);
        __riscv_vse32_v_f32m8(dst, vc, vl);
        sink(dst);
    }
    uint64_t t1 = read_cycles();

    double avg = (double)(t1 - t0) / ITERATIONS;
    printf("  avg cycles/iter = %.1f  (vl=%zu)\n", avg, vl);

    /* Correctness */
    float *ref = (float *)malloc(vl * sizeof(float));
    ref_vmul(src_a, src_b, ref, (int)vl);
    int ok = check_close(dst, ref, (int)vl, 1e-5f);
    printf("  correctness: %s\n\n", ok ? "PASS" : "FAIL");
    free(ref);
}

/* ---------- test 2: vle32 -> vfmacc (axpy, L1-resident) ------------------ */
static void
test_load_macc(const float *src_a, const float *src_b,
               float *dst, int n)
{
    printf("--- Test 2: vle32 -> vfmacc / axpy (L1-resident, LMUL=m8) ---\n");

    size_t vl = __riscv_vsetvlmax_e32m8();
    if ((int)vl > n) vl = (size_t)n;

    vfloat32m8_t acc = __riscv_vfmv_v_f_f32m8(0.0f, vl);
    vfloat32m8_t vb  = __riscv_vle32_v_f32m8(src_b, vl);

    /* Warmup — prime the cache and pipeline; result is discarded by the
     * reset below so it does not affect the reference calculation. */
    for (int i = 0; i < WARMUP; i++) {
        vfloat32m8_t va = __riscv_vle32_v_f32m8(src_a, vl);
        acc = __riscv_vfmacc_vv_f32m8(acc, va, vb, vl);
    }

    /* Reset accumulator — warmup result is intentionally discarded */
    acc = __riscv_vfmv_v_f_f32m8(0.0f, vl);

    uint64_t t0 = read_cycles();
    for (int it = 0; it < ITERATIONS; it++) {
        /* Chain: load a, then acc += a * b  (vb already in register) */
        vfloat32m8_t va = __riscv_vle32_v_f32m8(src_a, vl);
        acc = __riscv_vfmacc_vv_f32m8(acc, va, vb, vl);
    }
    uint64_t t1 = read_cycles();

    double avg = (double)(t1 - t0) / ITERATIONS;
    printf("  avg cycles/iter = %.1f  (vl=%zu)\n", avg, vl);

    /* Write back for correctness check */
    __riscv_vse32_v_f32m8(dst, acc, vl);

    float *ref_acc = (float *)calloc(vl, sizeof(float));
    /* acc was reset to 0 after warmup, so only ITERATIONS of accumulation */
    ref_vfmacc(src_a, src_b, ref_acc, (int)vl, ITERATIONS);
    int ok = check_close(dst, ref_acc, (int)vl, 1e-4f);
    printf("  correctness: %s\n\n", ok ? "PASS" : "FAIL");
    free(ref_acc);
}

/* ---------- test 3: vfmul baseline (no load in chain) -------------------- */
static void
test_mul_baseline(const float *src_a, const float *src_b, float *dst, int n)
{
    printf("--- Test 3: vfmul baseline (operands pre-loaded, no load chain) ---\n");

    size_t vl = __riscv_vsetvlmax_e32m8();
    if ((int)vl > n) vl = (size_t)n;

    vfloat32m8_t va = __riscv_vle32_v_f32m8(src_a, vl);
    vfloat32m8_t vb = __riscv_vle32_v_f32m8(src_b, vl);

    /* Warmup — store result to dst to prevent the compiler eliminating the
     * computation; vse32 has no side-effect on va/vb so the next multiply
     * still has no load-to-use dependency. */
    for (int i = 0; i < WARMUP; i++) {
        vfloat32m8_t vc = __riscv_vfmul_vv_f32m8(va, vb, vl);
        __riscv_vse32_v_f32m8(dst, vc, vl);
        sink(dst);
    }

    uint64_t t0 = read_cycles();
    for (int it = 0; it < ITERATIONS; it++) {
        vfloat32m8_t vc = __riscv_vfmul_vv_f32m8(va, vb, vl);
        __riscv_vse32_v_f32m8(dst, vc, vl);
    }
    uint64_t t1 = read_cycles();

    double avg = (double)(t1 - t0) / ITERATIONS;
    printf("  avg cycles/iter = %.1f  (vl=%zu)\n", avg, vl);
    printf("  (compare with Test 1; difference = load-chain overhead)\n\n");
}

/* ---------- main ---------------------------------------------------------- */
int
main(void)
{
    float *src_a = (float *)aligned_alloc(64, N * sizeof(float));
    float *src_b = (float *)aligned_alloc(64, N * sizeof(float));
    float *dst   = (float *)aligned_alloc(64, N * sizeof(float));

    if (!src_a || !src_b || !dst) {
        fprintf(stderr, "allocation failed\n");
        return 1;
    }

    for (int i = 0; i < N; i++) {
        src_a[i] = (float)(i + 1);
        src_b[i] = 2.0f;
        dst[i]   = 0.0f;
    }

    printf("=== Vector Load-Compute Chaining Test ===\n");
    printf("N=%d, LMUL=m8, SEW=32, ITERATIONS=%d\n\n", N, ITERATIONS);

    test_load_mul (src_a, src_b, dst, N);
    test_load_macc(src_a, src_b, dst, N);
    test_mul_baseline(src_a, src_b, dst, N);

    printf("=== Done ===\n");

    free(src_a);
    free(src_b);
    free(dst);
    return 0;
}
