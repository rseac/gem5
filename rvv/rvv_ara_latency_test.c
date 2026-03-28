/*
 * rvv_ara_latency_test.c
 *
 * Measures per-instruction chaining latency for each ARA VFU category.
 * Runs on ARA RTL simulation or gem5 AraO3 — no simulator-specific APIs
 * are used; only standard RISC-V (rdcycle CSR) and C99 stdio.
 *
 * Methodology
 * -----------
 * Each test creates a RAW dependency chain of CHAIN_LEN iterations:
 *
 *   v1 = op(v1, v2)   // output feeds next iteration's input
 *
 * When the vector is long enough that the pipeline throughput exceeds the
 * chaining latency, the hardware (or model) allows the consumer to start as
 * soon as the first result element is available.  avg_cycles/iter therefore
 * converges to the chaining latency of the instruction.
 *
 * Vector lengths
 * --------------
 * All tests use LMUL=m8 (16 throughput cycles at VLEN=512, 4 lanes).
 * Integer divide uses LMUL=m1: for the serial divider, dynamicOpLatency ==
 * chainingLatency (18 for EW32, 34 for EW64), giving a clean measurement.
 * The vdiv chain also includes a vadd_vx to keep values non-zero; on ARA
 * hardware the vadd adds its own chaining latency (6 cy) to the raw reading.
 *
 * Expected values on ARA RTL (VLEN=512, 4 lanes,
 *                              chainingLatency = max(pipe+2, DISPATCH_FLOOR=6)):
 *
 *   vadd_ew32    6   (ALU  pipe=1; max(3,6)=6)
 *   vmul_ew32    6   (Mul  pipe=1; max(3,6)=6)
 *   vdiv_ew32   24   (vdiv(18) + vadd(6) per iter)
 *   vdiv_ew64   40   (vdiv(34) + vadd(6) per iter)
 *   vfadd_ew32   6   (FPComp EW32 pipe=4; max(6,6)=6)
 *   vfadd_ew64   7   (FPComp EW64 pipe=5; max(7,6)=7)
 *   vfmul_ew32   6   (FPComp EW32, same as vfadd_ew32)
 *   vfmacc_ew32  6   (FPComp EW32, same as vfadd_ew32)
 *   vfmin_ew32   6   (FPNonComp pipe=1; max(3,6)=6)
 *   vfdiv_ew32   6   (FPDivSqrt pipe=3; max(5,6)=6)
 *   vfsqrt_ew32  6   (FPDivSqrt pipe=3; max(5,6)=6)
 *   vfcvt_ew32   6   (FPConv pipe=2; max(4,6)=6, per-op avg)
 *
 * Build (GCC, Linux toolchain):
 *   riscv64-unknown-linux-gnu-gcc -O3 -march=rv64gcv -mabi=lp64d \
 *       -static -o rvv_ara_latency_test.bin rvv_ara_latency_test.c
 *
 * Build (Clang, bare-metal / ARA runtime):
 *   clang -O3 -march=rv64gcv_zfh -mabi=lp64d -mcmodel=medany \
 *       -mno-relax -fuse-ld=lld -nostartfiles -Tlink.ld \
 *       -o rvv_ara_latency_test.bin rvv_ara_latency_test.c
 */

#include <riscv_vector.h>
#include <stdint.h>
#include <stdio.h>

/* ---------- cycle counter ------------------------------------------------ */
static inline uint64_t
read_cycles(void)
{
    uint64_t v;
    asm volatile("rdcycle %0" : "=r"(v));
    return v;
}

/* ---------- parameters --------------------------------------------------- */
#define CHAIN_LEN   100     /* dependency-chain length per test              */
#define WARMUP        8     /* iterations discarded before timing            */

/* Prevent the compiler from eliminating the final value of a vector reg.
 * Storing to a volatile pointer is the portable way on RISC-V.           */
static volatile float  _fsink;
static volatile int32_t _isink;

static void fsink_vec(vfloat32m8_t v, size_t vl) {
    _fsink = __riscv_vfmv_f_s_f32m8_f32(v);
    (void)vl;
}
static void fsink_vec64(vfloat64m8_t v, size_t vl) {
    _fsink = (float)__riscv_vfmv_f_s_f64m8_f64(v);
    (void)vl;
}
static void fsink_vec64_m1(vfloat64m1_t v, size_t vl) {
    _fsink = (float)__riscv_vfmv_f_s_f64m1_f64(v);
    (void)vl;
}
static void isink_vec(vint32m8_t v, size_t vl) {
    _isink = __riscv_vmv_x_s_i32m8_i32(v);
    (void)vl;
}
static void isink_vec_m1(vint32m1_t v, size_t vl) {
    _isink = __riscv_vmv_x_s_i32m1_i32(v);
    (void)vl;
}
static void isink_vec64_m1(vint64m1_t v, size_t vl) {
    _isink = (int32_t)__riscv_vmv_x_s_i64m1_i64(v);
    (void)vl;
}

/* Emit one result line in the format parsed by check_ara_latencies.py */
static void report(const char *name, uint64_t cycles, int iters)
{
    printf("LATENCY %s: avg=%.2f\n", name, (double)cycles / iters);
}

/* =========================================================================
 * Integer ALU — VFU_Alu, pipeline=1 cycle, chainingLatency=6
 * ======================================================================= */
static void
test_vadd_ew32(void)
{
    size_t vl = __riscv_vsetvlmax_e32m8();
    vint32m8_t v1 = __riscv_vmv_v_x_i32m8(1, vl);
    vint32m8_t v2 = __riscv_vmv_v_x_i32m8(1, vl);

    for (int i = 0; i < WARMUP; i++)
        v1 = __riscv_vadd_vv_i32m8(v1, v2, vl);

    uint64_t t0 = read_cycles();
    for (int i = 0; i < CHAIN_LEN; i++)
        v1 = __riscv_vadd_vv_i32m8(v1, v2, vl);
    uint64_t t1 = read_cycles();

    isink_vec(v1, vl);
    report("vadd_ew32", t1 - t0, CHAIN_LEN);
}

/* =========================================================================
 * Integer Multiply — VFU_Mul, pipeline=1 cycle (EW32), chainingLatency=6
 * ======================================================================= */
static void
test_vmul_ew32(void)
{
    size_t vl = __riscv_vsetvlmax_e32m8();
    vint32m8_t v1 = __riscv_vmv_v_x_i32m8(3, vl);
    vint32m8_t v2 = __riscv_vmv_v_x_i32m8(1, vl);  /* multiply by 1: no overflow */

    for (int i = 0; i < WARMUP; i++)
        v1 = __riscv_vmul_vv_i32m8(v1, v2, vl);

    uint64_t t0 = read_cycles();
    for (int i = 0; i < CHAIN_LEN; i++)
        v1 = __riscv_vmul_vv_i32m8(v1, v2, vl);
    uint64_t t1 = read_cycles();

    isink_vec(v1, vl);
    report("vmul_ew32", t1 - t0, CHAIN_LEN);
}

/* =========================================================================
 * Integer Divide EW32 — VFU_Div, LMUL=m1
 * dynamicOpLatency = chainingLatency = max(16+2, 6) = 18
 * ======================================================================= */
static void
test_vdiv_ew32(void)
{
    /* LMUL=m1 so dynamicOpLatency == chainingLatency (both 18).
     * Divisor=3 gives ~log2(3)≈2 iterations in the serial divider,
     * representative of a moderate-latency case. */
    size_t vl = __riscv_vsetvl_e32m1(4);   /* small vl inside m1 */
    vint32m1_t v1 = __riscv_vmv_v_x_i32m1(1000, vl);
    vint32m1_t v2 = __riscv_vmv_v_x_i32m1(3, vl);

    /* Warmup — bring divider out of any reset state */
    for (int i = 0; i < WARMUP; i++) {
        v1 = __riscv_vdiv_vv_i32m1(v1, v2, vl);
        /* prevent compiler from assuming v1 == const after first div */
        v1 = __riscv_vadd_vx_i32m1(v1, 1, vl);
    }

    /* Reset to a stable non-zero value */
    v1 = __riscv_vmv_v_x_i32m1(1 << 30, vl);

    uint64_t t0 = read_cycles();
    for (int i = 0; i < CHAIN_LEN; i++) {
        v1 = __riscv_vdiv_vv_i32m1(v1, v2, vl);
        /* keep v1 non-zero so the chain remains meaningful */
        v1 = __riscv_vadd_vx_i32m1(v1, 1, vl);
    }
    uint64_t t1 = read_cycles();

    isink_vec_m1(v1, vl);
    /* Each "step" = vdiv + vadd; report vdiv share only.
     * vadd is 6 cycles, so vdiv_cycles ≈ (total - CHAIN_LEN*6) / CHAIN_LEN.
     * But we report the raw average and let the checker correct for vadd. */
    report("vdiv_ew32", t1 - t0, CHAIN_LEN);
}

/* =========================================================================
 * Integer Divide EW64 — VFU_Div, LMUL=m1
 * dynamicOpLatency = chainingLatency = max(32+2, 6) = 34
 * ======================================================================= */
static void
test_vdiv_ew64(void)
{
    size_t vl = __riscv_vsetvl_e64m1(2);
    vint64m1_t v1 = __riscv_vmv_v_x_i64m1((int64_t)1 << 60, vl);
    vint64m1_t v2 = __riscv_vmv_v_x_i64m1(3, vl);

    for (int i = 0; i < WARMUP; i++) {
        v1 = __riscv_vdiv_vv_i64m1(v1, v2, vl);
        v1 = __riscv_vadd_vx_i64m1(v1, 1, vl);
    }

    v1 = __riscv_vmv_v_x_i64m1((int64_t)1 << 60, vl);

    uint64_t t0 = read_cycles();
    for (int i = 0; i < CHAIN_LEN; i++) {
        v1 = __riscv_vdiv_vv_i64m1(v1, v2, vl);
        v1 = __riscv_vadd_vx_i64m1(v1, 1, vl);
    }
    uint64_t t1 = read_cycles();

    isink_vec64_m1(v1, vl);
    report("vdiv_ew64", t1 - t0, CHAIN_LEN);
}

/* =========================================================================
 * FP Add EW32 — VFU_MFpu, LatFCompEW32=4, chainingLatency=max(6,6)=6
 * ======================================================================= */
static void
test_vfadd_ew32(void)
{
    size_t vl = __riscv_vsetvlmax_e32m8();
    vfloat32m8_t v1 = __riscv_vfmv_v_f_f32m8(1.0f, vl);
    vfloat32m8_t v2 = __riscv_vfmv_v_f_f32m8(0.0f, vl);  /* add 0: stable */

    for (int i = 0; i < WARMUP; i++)
        v1 = __riscv_vfadd_vv_f32m8(v1, v2, vl);

    uint64_t t0 = read_cycles();
    for (int i = 0; i < CHAIN_LEN; i++)
        v1 = __riscv_vfadd_vv_f32m8(v1, v2, vl);
    uint64_t t1 = read_cycles();

    fsink_vec(v1, vl);
    report("vfadd_ew32", t1 - t0, CHAIN_LEN);
}

/* =========================================================================
 * FP Add EW64 — VFU_MFpu, LatFCompEW64=5, chainingLatency=max(7,6)=7
 * The only FP test distinguishable from the DISPATCH_FLOOR.
 * ======================================================================= */
static void
test_vfadd_ew64(void)
{
    size_t vl = __riscv_vsetvlmax_e64m8();
    vfloat64m8_t v1 = __riscv_vfmv_v_f_f64m8(1.0, vl);
    vfloat64m8_t v2 = __riscv_vfmv_v_f_f64m8(0.0, vl);

    for (int i = 0; i < WARMUP; i++)
        v1 = __riscv_vfadd_vv_f64m8(v1, v2, vl);

    uint64_t t0 = read_cycles();
    for (int i = 0; i < CHAIN_LEN; i++)
        v1 = __riscv_vfadd_vv_f64m8(v1, v2, vl);
    uint64_t t1 = read_cycles();

    fsink_vec64(v1, vl);
    report("vfadd_ew64", t1 - t0, CHAIN_LEN);
}

/* =========================================================================
 * FP Multiply EW32 — VFU_MFpu, LatFCompEW32=4, chainingLatency=6
 * ======================================================================= */
static void
test_vfmul_ew32(void)
{
    size_t vl = __riscv_vsetvlmax_e32m8();
    vfloat32m8_t v1 = __riscv_vfmv_v_f_f32m8(1.0f, vl);
    vfloat32m8_t v2 = __riscv_vfmv_v_f_f32m8(1.0f, vl);  /* mul by 1: stable */

    for (int i = 0; i < WARMUP; i++)
        v1 = __riscv_vfmul_vv_f32m8(v1, v2, vl);

    uint64_t t0 = read_cycles();
    for (int i = 0; i < CHAIN_LEN; i++)
        v1 = __riscv_vfmul_vv_f32m8(v1, v2, vl);
    uint64_t t1 = read_cycles();

    fsink_vec(v1, vl);
    report("vfmul_ew32", t1 - t0, CHAIN_LEN);
}

/* =========================================================================
 * FP Multiply-Accumulate EW32 — vfmacc: vd += vs1 * vs2
 * chain is on vd; chainingLatency=6
 * ======================================================================= */
static void
test_vfmacc_ew32(void)
{
    size_t vl = __riscv_vsetvlmax_e32m8();
    vfloat32m8_t vd = __riscv_vfmv_v_f_f32m8(0.0f, vl);
    vfloat32m8_t vs1 = __riscv_vfmv_v_f_f32m8(1e-4f, vl);  /* small: no overflow */
    vfloat32m8_t vs2 = __riscv_vfmv_v_f_f32m8(1.0f, vl);

    for (int i = 0; i < WARMUP; i++)
        vd = __riscv_vfmacc_vv_f32m8(vd, vs1, vs2, vl);

    uint64_t t0 = read_cycles();
    for (int i = 0; i < CHAIN_LEN; i++)
        vd = __riscv_vfmacc_vv_f32m8(vd, vs1, vs2, vl);
    uint64_t t1 = read_cycles();

    fsink_vec(vd, vl);
    report("vfmacc_ew32", t1 - t0, CHAIN_LEN);
}

/* =========================================================================
 * FP Non-Computational (vfmin) EW32 — LatFNonComp=1, chainingLatency=6
 * ======================================================================= */
static void
test_vfmin_ew32(void)
{
    size_t vl = __riscv_vsetvlmax_e32m8();
    vfloat32m8_t v1 = __riscv_vfmv_v_f_f32m8(1.0f, vl);
    vfloat32m8_t v2 = __riscv_vfmv_v_f_f32m8(2.0f, vl);  /* min(v1,v2)=v1: stable */

    for (int i = 0; i < WARMUP; i++)
        v1 = __riscv_vfmin_vv_f32m8(v1, v2, vl);

    uint64_t t0 = read_cycles();
    for (int i = 0; i < CHAIN_LEN; i++)
        v1 = __riscv_vfmin_vv_f32m8(v1, v2, vl);
    uint64_t t1 = read_cycles();

    fsink_vec(v1, vl);
    report("vfmin_ew32", t1 - t0, CHAIN_LEN);
}

/* =========================================================================
 * FP Divide EW32 — VFU_MFpu PULP unit, LatFDivSqrt=3, chainingLatency=6
 * v1 = v1 / v2;  divide by 1.0 keeps v1 stable
 * ======================================================================= */
static void
test_vfdiv_ew32(void)
{
    size_t vl = __riscv_vsetvlmax_e32m8();
    vfloat32m8_t v1 = __riscv_vfmv_v_f_f32m8(4.0f, vl);
    vfloat32m8_t v2 = __riscv_vfmv_v_f_f32m8(1.0f, vl);

    for (int i = 0; i < WARMUP; i++)
        v1 = __riscv_vfdiv_vv_f32m8(v1, v2, vl);

    uint64_t t0 = read_cycles();
    for (int i = 0; i < CHAIN_LEN; i++)
        v1 = __riscv_vfdiv_vv_f32m8(v1, v2, vl);
    uint64_t t1 = read_cycles();

    fsink_vec(v1, vl);
    report("vfdiv_ew32", t1 - t0, CHAIN_LEN);
}

/* =========================================================================
 * FP Square Root EW32 — LatFDivSqrt=3, chainingLatency=6
 * sqrt converges to 1.0 quickly; result stays ≥0 for sqrt
 * ======================================================================= */
static void
test_vfsqrt_ew32(void)
{
    size_t vl = __riscv_vsetvlmax_e32m8();
    /* Use 1.0 so sqrt(1)=1: perfectly stable chain */
    vfloat32m8_t v1 = __riscv_vfmv_v_f_f32m8(1.0f, vl);

    for (int i = 0; i < WARMUP; i++)
        v1 = __riscv_vfsqrt_v_f32m8(v1, vl);

    uint64_t t0 = read_cycles();
    for (int i = 0; i < CHAIN_LEN; i++)
        v1 = __riscv_vfsqrt_v_f32m8(v1, vl);
    uint64_t t1 = read_cycles();

    fsink_vec(v1, vl);
    report("vfsqrt_ew32", t1 - t0, CHAIN_LEN);
}

/* =========================================================================
 * FP Conversion EW32 — LatFConv=2, chainingLatency=max(4,6)=6
 * Alternating int32↔float32 chain: each pair = 2 ops.
 * Run CHAIN_LEN pairs → CHAIN_LEN*2 individual ops; report per-op average.
 * ======================================================================= */
static void
test_vfcvt_ew32(void)
{
    size_t vl = __riscv_vsetvlmax_e32m8();
    vint32m8_t   vi = __riscv_vmv_v_x_i32m8(1000, vl);
    vfloat32m8_t vf = __riscv_vfmv_v_f_f32m8(0.0f, vl);

    /* Warmup — alternating conversions */
    for (int i = 0; i < WARMUP; i++) {
        vf = __riscv_vfcvt_f_x_v_f32m8(vi, vl);   /* int32 → float32 */
        vi = __riscv_vfcvt_x_f_v_i32m8(vf, vl);   /* float32 → int32 */
    }

    uint64_t t0 = read_cycles();
    for (int i = 0; i < CHAIN_LEN; i++) {
        vf = __riscv_vfcvt_f_x_v_f32m8(vi, vl);
        vi = __riscv_vfcvt_x_f_v_i32m8(vf, vl);
    }
    uint64_t t1 = read_cycles();

    isink_vec(vi, vl);
    /* CHAIN_LEN pairs = CHAIN_LEN*2 individual conversions */
    report("vfcvt_ew32", t1 - t0, CHAIN_LEN * 2);
}

/* =========================================================================
 * FP Non-Compute EW64 — vfmin, LatFNonComp=1, chainingLatency=max(3,6)=6
 * Exercises SimdFloatAluOp at EW64 where the bug was masked by DISPATCH_FLOOR
 * at EW32 but gives chainingLatency=7 (wrong) vs 6 (correct) at EW64.
 * ======================================================================= */
static void
test_vfmin_ew64(void)
{
    size_t vl = __riscv_vsetvlmax_e64m8();
    vfloat64m8_t v1 = __riscv_vfmv_v_f_f64m8(1.0, vl);
    vfloat64m8_t v2 = __riscv_vfmv_v_f_f64m8(2.0, vl);  /* min(v1,v2)=v1: stable */

    for (int i = 0; i < WARMUP; i++)
        v1 = __riscv_vfmin_vv_f64m8(v1, v2, vl);

    uint64_t t0 = read_cycles();
    for (int i = 0; i < CHAIN_LEN; i++)
        v1 = __riscv_vfmin_vv_f64m8(v1, v2, vl);
    uint64_t t1 = read_cycles();

    fsink_vec64(v1, vl);
    report("vfmin_ew64", t1 - t0, CHAIN_LEN);
}

/* =========================================================================
 * FP Compare EW64 — vmfeq, SimdFloatCmpOp, LatFComp*=5 at EW64
 * chainingLatency = max(5+2, 6) = 7
 *
 * Chain: vmfeq_vv (EW64) → vfmerge_vfm (EW64) → vmfeq_vv → ...
 *
 *   vbool8_t mask = vmfeq(v1, v2)     writes mask; CL = 7 (SimdFloatCmpOp EW64)
 *   v1            = vfmerge(v1,1.0,mask)  reads mask RAW; CL = 6 (SimdFloatAluOp)
 *   next vmfeq reads v1 RAW from vfmerge
 *
 * Per-iteration spacing = CL(vmfeq) + CL(vfmerge) = 7 + 6 = 13.
 * Without the fix (CL_vmfeq=6): spacing = 6+6=12 — measurably different.
 *
 * v1 and v2 both hold 1.0 so vmfeq always produces all-ones mask and
 * vfmerge always returns 1.0 for every element (stable chain).
 * ======================================================================= */
/* Sink helper for f64m1 (used by vmfeq test) */
static void fsink_vec64_m1_local(vfloat64m1_t v, size_t vl) {
    _fsink = (float)__riscv_vfmv_f_s_f64m1_f64(v); (void)vl;
}

static void
test_vmfeq_ew64(void)
{
    /* Use LMUL=m1 so there are only 2 micro-ops per instruction.
     * With m8 the 16 micro-op chain dilutes the 1-cycle CL effect.
     * With m1: spacing = CL(vmfeq_ew64) + CL(vfmerge_ew64) = 7+6=13
     * vs broken CL=6: 6+6=12.  One-cycle difference is measurable.
     *
     * Chain: vmfeq(v1,v2) → writes mask_m1 (CL=7)
     *        vfmerge(v1,1.0,mask) → writes v1 (CL=6)
     *        next vmfeq reads v1
     * v1==v2==1.0 always → mask all-ones → v1 stable at 1.0.
     */
    size_t vl = __riscv_vsetvlmax_e64m1();          /* 8 elements at VLEN=512 */
    vfloat64m1_t v1 = __riscv_vfmv_v_f_f64m1(1.0, vl);
    vfloat64m1_t v2 = __riscv_vfmv_v_f_f64m1(1.0, vl);

    for (int i = 0; i < WARMUP; i++) {
        vbool64_t mask = __riscv_vmfeq_vv_f64m1_b64(v1, v2, vl);
        v1 = __riscv_vfmerge_vfm_f64m1(v1, 1.0, mask, vl);
    }

    uint64_t t0 = read_cycles();
    for (int i = 0; i < CHAIN_LEN; i++) {
        vbool64_t mask = __riscv_vmfeq_vv_f64m1_b64(v1, v2, vl);
        v1 = __riscv_vfmerge_vfm_f64m1(v1, 1.0, mask, vl);
    }
    uint64_t t1 = read_cycles();

    fsink_vec64_m1_local(v1, vl);
    /* CL(vmfeq_ew64)=7 + CL(vfmerge_ew64)=6 = 13 per iteration (plus O3 overhead).
     * Tests SimdFloatCmpOp at EW64: RTL fpu_latency() default → LatFCompEW64=5.
     * Without the fix (pipeline_lat=1) spacing = 6+6=12 — 1 cycle less. */
    report("vmfeq_ew64", t1 - t0, CHAIN_LEN);
}

/* =========================================================================
 * FP Sum Reduction EW64 — vfredusum, uses FP compute pipeline (LatFCompEW64=5)
 * chainingLatency = max(5+2, 6) = 7
 * Chain: sum reduce into scalar, broadcast back, reduce again.
 * ======================================================================= */
static void
test_vfredusum_ew64(void)
{
    size_t vl = __riscv_vsetvlmax_e64m1();
    vfloat64m1_t v1  = __riscv_vfmv_v_f_f64m1(1.0, vl);
    vfloat64m1_t acc = __riscv_vfmv_v_f_f64m1(0.0, vl);

    for (int i = 0; i < WARMUP; i++)
        acc = __riscv_vfredusum_vs_f64m1_f64m1(v1, acc, vl);

    uint64_t t0 = read_cycles();
    for (int i = 0; i < CHAIN_LEN; i++)
        acc = __riscv_vfredusum_vs_f64m1_f64m1(v1, acc, vl);
    uint64_t t1 = read_cycles();

    fsink_vec64_m1(acc, vl);
    report("vfredusum_ew64", t1 - t0, CHAIN_LEN);
}

/* =========================================================================
 * main
 * ======================================================================= */
int
main(void)
{
    printf("=== ARA Instruction Latency Tests ===\n");
    printf("CHAIN_LEN=%d  WARMUP=%d\n\n", CHAIN_LEN, WARMUP);

    test_vadd_ew32();
    test_vmul_ew32();
    test_vdiv_ew32();
    test_vdiv_ew64();
    test_vfadd_ew32();
    test_vfadd_ew64();
    test_vfmul_ew32();
    test_vfmacc_ew32();
    test_vfmin_ew32();
    test_vfmin_ew64();
    test_vmfeq_ew64();
    test_vfredusum_ew64();
    test_vfdiv_ew32();
    test_vfsqrt_ew32();
    test_vfcvt_ew32();

    printf("\n=== Done ===\n");
    return 0;
}
