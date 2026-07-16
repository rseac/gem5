
/*
 * This is an executable test containing a number of loops to measure
 * the performance of a compiler. Arrays' length is LEN_1D by default
 * and if you want a different array length, you should replace every
 * LEN_1D by your desired number which must be a multiple of 40. If you
 * want to increase the number of loop calls to have a longer run time
 * you have to manipulate the constant value iterations. There is a dummy
 * function called in each loop to make all computations appear required.
 * The time to execute this function is included in the time measurement
 * for the output but it is neglectable.
 *
 *  The output includes three columns:
 *    Loop:        The name of the loop
 *    Time(Sec):     The time in seconds to run the loop
 *    Checksum:    The checksum calculated when the test has run
 *
 * In this version of the codelets arrays are static type.
 *
 * All functions/loops are taken from "TEST SUITE FOR VECTORIZING COMPILERS"
 * by David Callahan, Jack Dongarra and David Levine except those whose
 * functions' name have 4 digits.
 */

#include <time.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <inttypes.h>
#include <math.h>
#include <sys/time.h>
#include <riscv_vector.h>

#include "common.h"
#include "array_defs.h"

// array definitions
__attribute__((aligned(ARRAY_ALIGNMENT))) real_t flat_2d_array[LEN_2D*LEN_2D];

__attribute__((aligned(ARRAY_ALIGNMENT))) real_t x[LEN_1D];

__attribute__((aligned(ARRAY_ALIGNMENT))) real_t a[LEN_1D],b[LEN_1D],c[LEN_1D],d[LEN_1D],e[LEN_1D],
                                   aa[LEN_2D][LEN_2D],bb[LEN_2D][LEN_2D],cc[LEN_2D][LEN_2D],tt[LEN_2D][LEN_2D];

__attribute__((aligned(ARRAY_ALIGNMENT))) int indx[LEN_1D];

real_t* __restrict__ xx;
real_t* yy;

real_t s000(struct args_t * func_args)
{

//    linear dependence testing
//    no dependence - vectorizable

    initialise_arrays(__func__);
    ROI_BEGIN(func_args);

    for (int nl = 0; nl < 2*iterations; nl++) {
        for (int i = 0; i < LEN_1D; i++) {
            a[i] = b[i] + 1;
        }
        dummy((real_t*)a, (real_t*)b, (real_t*)c, (real_t*)d, (real_t*)e, aa, bb, cc, 0.);
    }

    ROI_END(func_args);
    return calc_checksum(__func__);
}

// %1.1
real_t s111(struct args_t * func_args)
{

//    linear dependence testing
//    no dependence - vectorizable

    initialise_arrays(__func__);
    ROI_BEGIN(func_args);

    for (int nl = 0; nl < 2*iterations; nl++) {
        for (int i = 1; i < LEN_1D; i += 2) {
            a[i] = a[i - 1] + b[i];
        }
        dummy(a, b, c, d, e, aa, bb, cc, 0.);
    }

    ROI_END(func_args);
    return calc_checksum(__func__);
}

real_t s1111(struct args_t * func_args)
{

//    no dependence - vectorizable
//    jump in data access

    initialise_arrays(__func__);
    ROI_BEGIN(func_args);

    for (int nl = 0; nl < 2*iterations; nl++) {
        for (int i = 0; i < LEN_1D/2; i++) {
            a[2*i] = c[i] * b[i] + d[i] * b[i] + c[i] * c[i] + d[i] * b[i] + d[i] * c[i];
        }
        dummy(a, b, c, d, e, aa, bb, cc, 0.);
    }

    ROI_END(func_args);
    return calc_checksum(__func__);
}

// %1.1

real_t s112(struct args_t * func_args)
{

//    linear dependence testing
//    loop reversal

    initialise_arrays(__func__);
    ROI_BEGIN(func_args);

    for (int nl = 0; nl < 3*iterations; nl++) {
        for (int i = LEN_1D - 2; i >= 0; i--) {
            a[i+1] = a[i] + b[i];
        }
        dummy(a, b, c, d, e, aa, bb, cc, 0.);
    }

    ROI_END(func_args);
    return calc_checksum(__func__);
}

real_t s1112(struct args_t * func_args)
{

//    linear dependence testing
//    loop reversal

    initialise_arrays(__func__);
    ROI_BEGIN(func_args);

    for (int nl = 0; nl < iterations*3; nl++) {
        for (int i = LEN_1D - 1; i >= 0; i--) {
            a[i] = b[i] + (real_t) 1.;
        }
        dummy(a, b, c, d, e, aa, bb, cc, 0.);
    }

    ROI_END(func_args);
    return calc_checksum(__func__);
}

// %1.1

real_t s113(struct args_t * func_args)
{

//    linear dependence testing
//    a(i)=a(1) but no actual dependence cycle

    initialise_arrays(__func__);
    ROI_BEGIN(func_args);

    for (int nl = 0; nl < 4*iterations; nl++) {
        for (int i = 1; i < LEN_1D; i++) {
            a[i] = a[0] + b[i];
        }
        dummy(a, b, c, d, e, aa, bb, cc, 0.);
    }

    ROI_END(func_args);
    return calc_checksum(__func__);
}

real_t s1113(struct args_t * func_args)
{

//    linear dependence testing
//    one iteration dependency on a(LEN_1D/2) but still vectorizable

    initialise_arrays(__func__);
    ROI_BEGIN(func_args);

    for (int nl = 0; nl < 2*iterations; nl++) {
        for (int i = 0; i < LEN_1D; i++) {
            a[i] = a[LEN_1D/2] + b[i];
        }
        dummy(a, b, c, d, e, aa, bb, cc, 0.);
    }

    ROI_END(func_args);
    return calc_checksum(__func__);
}

// %1.1

real_t s114(struct args_t * func_args)
{

//    linear dependence testing
//    transpose vectorization
//    Jump in data access - not vectorizable

    initialise_arrays(__func__);
    ROI_BEGIN(func_args);

    for (int nl = 0; nl < 200*(iterations/(LEN_2D)); nl++) {
        for (int i = 0; i < LEN_2D; i++) {
            for (int j = 0; j < i; j++) {
                aa[i][j] = aa[j][i] + bb[i][j];
            }
        }
        dummy(a, b, c, d, e, aa, bb, cc, 0.);
    }

    ROI_END(func_args);
    return calc_checksum(__func__);
}

// %1.1

real_t s115(struct args_t * func_args)
{

//    linear dependence testing
//    triangular saxpy loop

    initialise_arrays(__func__);
    ROI_BEGIN(func_args);

    for (int nl = 0; nl < 1000*(iterations/LEN_2D); nl++) {
        for (int j = 0; j < LEN_2D; j++) {
            for (int i = j+1; i < LEN_2D; i++) {
                a[i] -= aa[j][i] * a[j];
            }
        }
        dummy(a, b, c, d, e, aa, bb, cc, 0.);
    }

    ROI_END(func_args);
    return calc_checksum(__func__);
}

real_t s1115(struct args_t * func_args)
{

//    linear dependence testing
//    triangular saxpy loop

    initialise_arrays(__func__);
    ROI_BEGIN(func_args);

    for (int nl = 0; nl < 100*(iterations/LEN_2D); nl++) {
        for (int i = 0; i < LEN_2D; i++) {
            for (int j = 0; j < LEN_2D; j++) {
                aa[i][j] = aa[i][j]*cc[j][i] + bb[i][j];
            }
        }
        dummy(a, b, c, d, e, aa, bb, cc, 0.);
    }

    ROI_END(func_args);
    return calc_checksum(__func__);
}

// %1.1

real_t s116(struct args_t * func_args)
{

//    linear dependence testing

    initialise_arrays(__func__);
    ROI_BEGIN(func_args);

    for (int nl = 0; nl < iterations*10; nl++) {
        for (int i = 0; i < LEN_1D - 5; i += 5) {
            a[i] = a[i + 1] * a[i];
            a[i + 1] = a[i + 2] * a[i + 1];
            a[i + 2] = a[i + 3] * a[i + 2];
            a[i + 3] = a[i + 4] * a[i + 3];
            a[i + 4] = a[i + 5] * a[i + 4];
        }
        dummy(a, b, c, d, e, aa, bb, cc, 0.);
    }

    ROI_END(func_args);
    return calc_checksum(__func__);
}

// %1.1

real_t s118(struct args_t * func_args)
{

//    linear dependence testing
//    potential dot product recursion

    initialise_arrays(__func__);
    ROI_BEGIN(func_args);

    for (int nl = 0; nl < 200*(iterations/LEN_2D); nl++) {
        for (int i = 1; i < LEN_2D; i++) {
            for (int j = 0; j <= i - 1; j++) {
                a[i] += bb[j][i] * a[i-j-1];
            }
        }
        dummy(a, b, c, d, e, aa, bb, cc, 0.);
    }

    ROI_END(func_args);
    return calc_checksum(__func__);
}

// %1.1

real_t s119(struct args_t * func_args)
{

//    linear dependence testing
//    no dependence - vectorizable

    initialise_arrays(__func__);
    ROI_BEGIN(func_args);

    for (int nl = 0; nl < 200*(iterations/(LEN_2D)); nl++) {
        for (int i = 1; i < LEN_2D; i++) {
            for (int j = 1; j < LEN_2D; j++) {
                aa[i][j] = aa[i-1][j-1] + bb[i][j];
            }
        }
        dummy(a, b, c, d, e, aa, bb, cc, 0.);
    }

    ROI_END(func_args);
    return calc_checksum(__func__);
}

real_t s1119(struct args_t * func_args)
{

//    linear dependence testing
//    no dependence - vectorizable

    initialise_arrays(__func__);
    ROI_BEGIN(func_args);

    for (int nl = 0; nl < 200*(iterations/(LEN_2D)); nl++) {
        for (int i = 1; i < LEN_2D; i++) {
            for (int j = 0; j < LEN_2D; j++) {
                aa[i][j] = aa[i-1][j] + bb[i][j];
            }
        }
        dummy(a, b, c, d, e, aa, bb, cc, 0.);
    }

    ROI_END(func_args);
    return calc_checksum(__func__);
}

// %1.2

real_t s121(struct args_t * func_args)
{

//    induction variable recognition
//    loop with possible ambiguity because of scalar store

    initialise_arrays(__func__);
    ROI_BEGIN(func_args);

    int j;
    for (int nl = 0; nl < 3*iterations; nl++) {
        for (int i = 0; i < LEN_1D-1; i++) {
            j = i + 1;
            a[i] = a[j] + b[i];
        }
        dummy(a, b, c, d, e, aa, bb, cc, 0.);
    }

    ROI_END(func_args);
    return calc_checksum(__func__);
}

// %1.2

real_t s122(struct args_t * func_args)
{

//    induction variable recognition
//    variable lower and upper bound, and stride
//    reverse data access and jump in data access

    struct{int a;int b;} * x = func_args->arg_info;
    int n1 = x->a;
    int n3 = x->b;

    initialise_arrays(__func__);
    ROI_BEGIN(func_args);

    int j, k;
    for (int nl = 0; nl < iterations; nl++) {
        j = 1;
        k = 0;
        for (int i = n1-1; i < LEN_1D; i += n3) {
            k += j;
            a[i] += b[LEN_1D - k];
        }
        dummy(a, b, c, d, e, aa, bb, cc, 0.);
    }

    ROI_END(func_args);
    return calc_checksum(__func__);
}

// %1.2

real_t s123(struct args_t * func_args)
{

//    induction variable recognition
//    induction variable under an if
//    not vectorizable, the condition cannot be speculated

    initialise_arrays(__func__);
    ROI_BEGIN(func_args);

    int j;
    for (int nl = 0; nl < iterations; nl++) {
        j = -1;
        for (int i = 0; i < (LEN_1D/2); i++) {
            j++;
            a[j] = b[i] + d[i] * e[i];
            if (c[i] > (real_t)0.) {
                j++;
                a[j] = c[i] + d[i] * e[i];
            }
        }
        dummy(a, b, c, d, e, aa, bb, cc, 0.);
    }

    ROI_END(func_args);
    return calc_checksum(__func__);
}

// %1.2

real_t s124(struct args_t * func_args)
{

//    induction variable recognition
//    induction variable under both sides of if (same value)

    initialise_arrays(__func__);
    ROI_BEGIN(func_args);

    int j;
    for (int nl = 0; nl < iterations; nl++) {
        j = -1;
        for (int i = 0; i < LEN_1D; i++) {
            if (b[i] > (real_t)0.) {
                j++;
                a[j] = b[i] + d[i] * e[i];
            } else {
                j++;
                a[j] = c[i] + d[i] * e[i];
            }
        }
        dummy(a, b, c, d, e, aa, bb, cc, 0.);
    }

    ROI_END(func_args);
    return calc_checksum(__func__);
}

// %1.2
real_t s125(struct args_t * func_args)
{

//    induction variable recognition
//    induction variable in two loops; collapsing possible

    initialise_arrays(__func__);
    ROI_BEGIN(func_args);

    int k;
    for (int nl = 0; nl < 100*(iterations/(LEN_2D)); nl++) {
        k = -1;
        for (int i = 0; i < LEN_2D; i++) {
            for (int j = 0; j < LEN_2D; j++) {
                k++;
                flat_2d_array[k] = aa[i][j] + bb[i][j] * cc[i][j];
            }
        }
        dummy(a, b, c, d, e, aa, bb, cc, 0.);
    }

    ROI_END(func_args);
    return calc_checksum(__func__);
}

// %1.2
real_t s126(struct args_t * func_args)
{

//    induction variable recognition
//    induction variable in two loops; recurrence in inner loop

    initialise_arrays(__func__);
    ROI_BEGIN(func_args);

    int k;
    for (int nl = 0; nl < 10*(iterations/LEN_2D); nl++) {
        k = 1;
        for (int i = 0; i < LEN_2D; i++) {
            for (int j = 1; j < LEN_2D; j++) {
                bb[j][i] = bb[j-1][i] + flat_2d_array[k-1] * cc[j][i];
                ++k;
            }
            ++k;
        }
        dummy(a, b, c, d, e, aa, bb, cc, 0.);
    }

    ROI_END(func_args);
    return calc_checksum(__func__);
}

// %1.2

real_t s127(struct args_t * func_args)
{

//    induction variable recognition
//    induction variable with multiple increments

    initialise_arrays(__func__);
    ROI_BEGIN(func_args);

    int j;
    for (int nl = 0; nl < 2*iterations; nl++) {
        j = -1;
        for (int i = 0; i < LEN_1D/2; i++) {
            j++;
            a[j] = b[i] + c[i] * d[i];
            j++;
            a[j] = b[i] + d[i] * e[i];
        }
        dummy(a, b, c, d, e, aa, bb, cc, 0.);
    }

    ROI_END(func_args);
    return calc_checksum(__func__);
}

// %1.2

real_t s128(struct args_t * func_args)
{

//    induction variables
//    coupled induction variables
//    jump in data access

    initialise_arrays(__func__);
    ROI_BEGIN(func_args);

    int j, k;
    for (int nl = 0; nl < 2*iterations; nl++) {
        j = -1;
        for (int i = 0; i < LEN_1D/2; i++) {
            k = j + 1;
            a[i] = b[k] - d[i];
            j = k + 1;
            b[k] = a[i] + c[k];
        }
        dummy(a, b, c, d, e, aa, bb, cc, 1.);
    }

    ROI_END(func_args);
    return calc_checksum(__func__);
}

// %1.3

real_t s131(struct args_t * func_args)
{
//    global data flow analysis
//    forward substitution

    initialise_arrays(__func__);
    ROI_BEGIN(func_args);

    int m  = 1;
    for (int nl = 0; nl < 5*iterations; nl++) {
        for (int i = 0; i < LEN_1D - 1; i++) {
            a[i] = a[i + m] + b[i];
        }
        dummy(a, b, c, d, e, aa, bb, cc, 0.);
    }

    ROI_END(func_args);
    return calc_checksum(__func__);
}

// %1.3

real_t s132(struct args_t * func_args)
{
//    global data flow analysis
//    loop with multiple dimension ambiguous subscripts

    initialise_arrays(__func__);
    ROI_BEGIN(func_args);

    int m = 0;
    int j = m;
    int k = m+1;
    for (int nl = 0; nl < 400*iterations; nl++) {
        for (int i= 1; i < LEN_2D; i++) {
            aa[j][i] = aa[k][i-1] + b[i] * c[1];
        }
        dummy(a, b, c, d, e, aa, bb, cc, 0.);
    }

    ROI_END(func_args);
    return calc_checksum(__func__);
}

// %1.4

real_t s141(struct args_t * func_args)
{

//    nonlinear dependence testing
//    walk a row in a symmetric packed array
//    element a(i,j) for (int j>i) stored in location j*(j-1)/2+i

    initialise_arrays(__func__);
    ROI_BEGIN(func_args);

    int k;
    for (int nl = 0; nl < 200*(iterations/LEN_2D); nl++) {
        for (int i = 0; i < LEN_2D; i++) {
            k = (i+1) * ((i+1) - 1) / 2 + (i+1)-1;
            for (int j = i; j < LEN_2D; j++) {
                flat_2d_array[k] += bb[j][i];
                k += j+1;
            }
        }
        dummy(a, b, c, d, e, aa, bb, cc, 0.);
    }

    ROI_END(func_args);
    return calc_checksum(__func__);
}

// %1.5

void s151s(real_t a[LEN_1D], real_t b[LEN_1D],  int m)
{
    for (int i = 0; i < LEN_1D-1; i++) {
        a[i] = a[i + m] + b[i];
    }
}

real_t s151(struct args_t * func_args)
{

//    interprocedural data flow analysis
//    passing parameter information into a subroutine

    initialise_arrays(__func__);
    ROI_BEGIN(func_args);

    for (int nl = 0; nl < 5*iterations; nl++) {
        s151s(a, b,  1);
        dummy(a, b, c, d, e, aa, bb, cc, 0.);
    }

    ROI_END(func_args);
    return calc_checksum(__func__);
}

// %1.5

void s152s(real_t a[LEN_1D], real_t b[LEN_1D], real_t c[LEN_1D], int i)
{
    a[i] += b[i] * c[i];
}

real_t s152(struct args_t * func_args)
{

//    interprocedural data flow analysis
//    collecting information from a subroutine

    initialise_arrays(__func__);
    ROI_BEGIN(func_args);

    for (int nl = 0; nl < iterations; nl++) {
        for (int i = 0; i < LEN_1D; i++) {
            b[i] = d[i] * e[i];
            s152s(a, b, c, i);
        }
        dummy(a, b, c, d, e, aa, bb, cc, 0.);
    }

    ROI_END(func_args);
    return calc_checksum(__func__);
}

// %1.6

real_t s161(struct args_t * func_args)
{

//    control flow
//    tests for recognition of loop independent dependences
//    between statements in mutually exclusive regions.

    initialise_arrays(__func__);
    ROI_BEGIN(func_args);

    for (int nl = 0; nl < iterations/2; nl++) {
        for (int i = 0; i < LEN_1D-1; ++i) {
            if (b[i] < (real_t)0.) {
                goto L20;
            }
            a[i] = c[i] + d[i] * e[i];
            goto L10;
L20:
            c[i+1] = a[i] + d[i] * d[i];
L10:
            ;
        }
        dummy(a, b, c, d, e, aa, bb, cc, 0.);
    }

    ROI_END(func_args);
    return calc_checksum(__func__);
}

real_t s1161(struct args_t * func_args)
{

//    control flow
//    tests for recognition of loop independent dependences
//    between statements in mutually exclusive regions.

    initialise_arrays(__func__);
    ROI_BEGIN(func_args);

    for (int nl = 0; nl < iterations; nl++) {
        for (int i = 0; i < LEN_1D-1; ++i) {
            if (c[i] < (real_t)0.) {
                goto L20;
            }
            a[i] = c[i] + d[i] * e[i];
            goto L10;
L20:
            b[i] = a[i] + d[i] * d[i];
L10:
            ;
        }
        dummy(a, b, c, d, e, aa, bb, cc, 0.);
    }

    ROI_END(func_args);
    return calc_checksum(__func__);
}

// %1.6

//int s162(int k)
real_t s162(struct args_t * func_args)
{
//    control flow
//    deriving assertions

    int k = *(int*)func_args->arg_info;

    initialise_arrays(__func__);
    ROI_BEGIN(func_args);

    for (int nl = 0; nl < iterations; nl++) {
        if (k > 0) {
            for (int i = 0; i < LEN_1D-1; i++) {
                a[i] = a[i + k] + b[i] * c[i];
            }
        }
        dummy(a, b, c, d, e, aa, bb, cc, 0.);
    }

    ROI_END(func_args);
    return calc_checksum(__func__);
}

// %1.7

//int s171(int inc)
real_t s171(struct args_t * func_args)
{

//    symbolics
//    symbolic dependence tests

    int inc = *(int*)func_args->arg_info;

    initialise_arrays(__func__);
    ROI_BEGIN(func_args);

    for (int nl = 0; nl < iterations; nl++) {
        for (int i = 0; i < LEN_1D; i++) {
            a[i * inc] += b[i];
        }
        dummy(a, b, c, d, e, aa, bb, cc, 0.);
    }

    ROI_END(func_args);
    return calc_checksum(__func__);
}

// %1.7

//int s172( int n1, int n3)
real_t s172(struct args_t * func_args)
{
//    symbolics
//    vectorizable if n3 .ne. 0

    struct{int a;int b;} * x = func_args->arg_info;
    int n1 = x->a;
    int n3 = x->b;

    initialise_arrays(__func__);
    ROI_BEGIN(func_args);

    for (int nl = 0; nl < iterations; nl++) {
        for (int i = n1-1; i < LEN_1D; i += n3) {
            a[i] += b[i];
        }
        dummy(a, b, c, d, e, aa, bb, cc, 0.);
    }

    ROI_END(func_args);
    return calc_checksum(__func__);
}

// %1.7

real_t s173(struct args_t * func_args)
{
//    symbolics
//    expression in loop bounds and subscripts

    initialise_arrays(__func__);
    ROI_BEGIN(func_args);

    int k = LEN_1D/2;
    for (int nl = 0; nl < 10*iterations; nl++) {
        for (int i = 0; i < LEN_1D/2; i++) {
            a[i+k] = a[i] + b[i];
        }
        dummy(a, b, c, d, e, aa, bb, cc, 0.);
    }

    ROI_END(func_args);
    return calc_checksum(__func__);
}

// %1.7

//int s174(int M)
real_t s174(struct args_t * func_args)
{

//    symbolics
//    loop with subscript that may seem ambiguous

    int M = *(int*)func_args->arg_info;

    initialise_arrays(__func__);
    ROI_BEGIN(func_args);

    for (int nl = 0; nl < 10*iterations; nl++) {
        for (int i = 0; i < M; i++) {
            a[i+M] = a[i] + b[i];
        }
        dummy(a, b, c, d, e, aa, bb, cc, 0.);
    }

    ROI_END(func_args);
    return calc_checksum(__func__);
}

// %1.7

//int s175(int inc)
real_t s175(struct args_t * func_args)
{

//    symbolics
//    symbolic dependence tests

    int inc = *(int*)func_args->arg_info;

    initialise_arrays(__func__);
    ROI_BEGIN(func_args);

    for (int nl = 0; nl < iterations; nl++) {
        for (int i = 0; i < LEN_1D-1; i += inc) {
            a[i] = a[i + inc] + b[i];
        }
        dummy(a, b, c, d, e, aa, bb, cc, 0.);
    }

    ROI_END(func_args);
    return calc_checksum(__func__);
}

// %1.7

real_t s176(struct args_t * func_args)
{

//    symbolics
//    convolution

    initialise_arrays(__func__);
    ROI_BEGIN(func_args);

    int m = LEN_1D/2;
    for (int nl = 0; nl < 4*(iterations/LEN_1D); nl++) {
        for (int j = 0; j < (LEN_1D/2); j++) {
            for (int i = 0; i < m; i++) {
                a[i] += b[i+m-j-1] * c[j];
            }
        }
        dummy(a, b, c, d, e, aa, bb, cc, 0.);
    }

    ROI_END(func_args);
    return calc_checksum(__func__);
}

// **********************************************************
// *                                *
// *            VECTORIZATION                *
// *                                *
// **********************************************************

// %2.1

real_t s211(struct args_t * func_args)
{

//    statement reordering
//    statement reordering allows vectorization

    initialise_arrays(__func__);
    ROI_BEGIN(func_args);

    for (int nl = 0; nl < iterations; nl++) {
        for (int i = 1; i < LEN_1D-1; i++) {
            a[i] = b[i - 1] + c[i] * d[i];
            b[i] = b[i + 1] - e[i] * d[i];
        }
        dummy(a, b, c, d, e, aa, bb, cc, 0.);
    }

    ROI_END(func_args);
    return calc_checksum(__func__);
}

// %2.1

real_t s212(struct args_t * func_args)
{

//    statement reordering
//    dependency needing temporary

    initialise_arrays(__func__);
    ROI_BEGIN(func_args);

    for (int nl = 0; nl < iterations; nl++) {
        for (int i = 0; i < LEN_1D-1; i++) {
            a[i] *= c[i];
            b[i] += a[i + 1] * d[i];
        }
        dummy(a, b, c, d, e, aa, bb, cc, 0.);
    }

    ROI_END(func_args);
    return calc_checksum(__func__);
}

real_t s1213(struct args_t * func_args)
{

//    statement reordering
//    dependency needing temporary

    initialise_arrays(__func__);
    ROI_BEGIN(func_args);

    for (int nl = 0; nl < iterations; nl++) {
        for (int i = 1; i < LEN_1D-1; i++) {
            a[i] = b[i-1]+c[i];
            b[i] = a[i+1]*d[i];
        }
        dummy(a, b, c, d, e, aa, bb, cc, 0.);
    }

    ROI_END(func_args);
    return calc_checksum(__func__);
}

// %2.2

real_t s221(struct args_t * func_args)
{

//    loop distribution
//    loop that is partially recursive

    initialise_arrays(__func__);
    ROI_BEGIN(func_args);

    for (int nl = 0; nl < iterations/2; nl++) {
        for (int i = 1; i < LEN_1D; i++) {
            a[i] += c[i] * d[i];
            b[i] = b[i - 1] + a[i] + d[i];
        }
        dummy(a, b, c, d, e, aa, bb, cc, 0.);
    }

    ROI_END(func_args);
    return calc_checksum(__func__);
}

real_t s1221(struct args_t * func_args)
{

//    run-time symbolic resolution

    initialise_arrays(__func__);
    ROI_BEGIN(func_args);

    for (int nl = 0; nl < iterations; nl++) {
        for (int i = 4; i < LEN_1D; i++) {
            b[i] = b[i - 4] + a[i];
        }
        dummy(a, b, c, d, e, aa, bb, cc, 0.);
    }

    ROI_END(func_args);
    return calc_checksum(__func__);
}

// %2.2

real_t s222(struct args_t * func_args)
{

//    loop distribution
//    partial loop vectorizatio recurrence in middle

    initialise_arrays(__func__);
    ROI_BEGIN(func_args);

    for (int nl = 0; nl < iterations/2; nl++) {
        for (int i = 1; i < LEN_1D; i++) {
            a[i] += b[i] * c[i];
            e[i] = e[i - 1] * e[i - 1];
            a[i] -= b[i] * c[i];
        }
        dummy(a, b, c, d, e, aa, bb, cc, 0.);
    }

    ROI_END(func_args);
    return calc_checksum(__func__);
}

// %2.3

real_t s231(struct args_t * func_args)
{
//    loop interchange
//    loop with data dependency

    initialise_arrays(__func__);
    ROI_BEGIN(func_args);

    for (int nl = 0; nl < 100*(iterations/LEN_2D); nl++) {
        for (int i = 0; i < LEN_2D; ++i) {
            for (int j = 1; j < LEN_2D; j++) {
                aa[j][i] = aa[j - 1][i] + bb[j][i];
            }
        }
        dummy(a, b, c, d, e, aa, bb, cc, 0.);
    }

    ROI_END(func_args);
    return calc_checksum(__func__);
}

// %2.3

real_t s232(struct args_t * func_args)
{

//    loop interchange
//    interchanging of triangular loops

    initialise_arrays(__func__);
    ROI_BEGIN(func_args);

    for (int nl = 0; nl < 100*(iterations/(LEN_2D)); nl++) {
        for (int j = 1; j < LEN_2D; j++) {
            for (int i = 1; i <= j; i++) {
                aa[j][i] = aa[j][i-1]*aa[j][i-1]+bb[j][i];
            }
        }
        dummy(a, b, c, d, e, aa, bb, cc, 1.);
    }

    ROI_END(func_args);
    return calc_checksum(__func__);
}

real_t s1232(struct args_t * func_args)
{

//    loop interchange
//    interchanging of triangular loops

    initialise_arrays(__func__);
    ROI_BEGIN(func_args);

    for (int nl = 0; nl < 100*(iterations/LEN_2D); nl++) {
        for (int j = 0; j < LEN_2D; j++) {
            for (int i = j; i < LEN_2D; i++) {
                aa[i][j] = bb[i][j] + cc[i][j];
            }
        }
        dummy(a, b, c, d, e, aa, bb, cc, 1.);
    }

    ROI_END(func_args);
    return calc_checksum(__func__);
}

// %2.3

real_t s233(struct args_t * func_args)
{

//    loop interchange
//    interchanging with one of two inner loops

    initialise_arrays(__func__);
    ROI_BEGIN(func_args);

    for (int nl = 0; nl < 100*(iterations/LEN_2D); nl++) {
        for (int i = 1; i < LEN_2D; i++) {
            for (int j = 1; j < LEN_2D; j++) {
                aa[j][i] = aa[j-1][i] + cc[j][i];
            }
            for (int j = 1; j < LEN_2D; j++) {
                bb[j][i] = bb[j][i-1] + cc[j][i];
            }
        }
        dummy(a, b, c, d, e, aa, bb, cc, 0.);
    }

    ROI_END(func_args);
    return calc_checksum(__func__);
}

real_t s2233(struct args_t * func_args)
{

//    loop interchange
//    interchanging with one of two inner loops

    initialise_arrays(__func__);
    ROI_BEGIN(func_args);

    for (int nl = 0; nl < 100*(iterations/LEN_2D); nl++) {
        for (int i = 1; i < LEN_2D; i++) {
            for (int j = 1; j < LEN_2D; j++) {
                aa[j][i] = aa[j-1][i] + cc[j][i];
            }
            for (int j = 1; j < LEN_2D; j++) {
                bb[i][j] = bb[i-1][j] + cc[i][j];
            }
        }
        dummy(a, b, c, d, e, aa, bb, cc, 0.);
    }

    ROI_END(func_args);
    return calc_checksum(__func__);
}

// %2.3
real_t s235(struct args_t * func_args)
{

//    loop interchanging
//    imperfectly nested loops

    initialise_arrays(__func__);
    ROI_BEGIN(func_args);

    for (int nl = 0; nl < 200*(iterations/LEN_2D); nl++) {
        for (int i = 0; i < LEN_2D; i++) {
            a[i] += b[i] * c[i];
            for (int j = 1; j < LEN_2D; j++) {
                aa[j][i] = aa[j-1][i] + bb[j][i] * a[i];
            }
        }
        dummy(a, b, c, d, e, aa, bb, cc, 0.);
    }

    ROI_END(func_args);
    return calc_checksum(__func__);
}

// %2.4

real_t s241(struct args_t * func_args)
{

//    node splitting
//    preloading necessary to allow vectorization

    initialise_arrays(__func__);
    ROI_BEGIN(func_args);

    for (int nl = 0; nl < 2*iterations; nl++) {
        for (int i = 0; i < LEN_1D-1; i++) {
            a[i] = b[i] * c[i  ] * d[i];
            b[i] = a[i] * a[i+1] * d[i];
        }
        dummy(a, b, c, d, e, aa, bb, cc, 0.);
    }

    ROI_END(func_args);
    return calc_checksum(__func__);
}

// %2.4

//int s242(real_t s1, real_t s2)
real_t s242(struct args_t * func_args)
{

//    node splitting

    struct{real_t a;real_t b;} * x = func_args->arg_info;
    real_t s1 = x->a;
    real_t s2 = x->b;

    initialise_arrays(__func__);
    ROI_BEGIN(func_args);

    for (int nl = 0; nl < iterations/5; nl++) {
        for (int i = 1; i < LEN_1D; ++i) {
            a[i] = a[i - 1] + s1 + s2 + b[i] + c[i] + d[i];
        }
        dummy(a, b, c, d, e, aa, bb, cc, 0.);
    }

    ROI_END(func_args);
    return calc_checksum(__func__);
}

// %2.4

real_t s243(struct args_t * func_args)
{

//    node splitting
//    false dependence cycle breaking

    initialise_arrays(__func__);
    ROI_BEGIN(func_args);

    for (int nl = 0; nl < iterations; nl++) {
        for (int i = 0; i < LEN_1D-1; i++) {
            a[i] = b[i] + c[i  ] * d[i];
            b[i] = a[i] + d[i  ] * e[i];
            a[i] = b[i] + a[i+1] * d[i];
        }
        dummy(a, b, c, d, e, aa, bb, cc, 0.);
    }

    ROI_END(func_args);
    return calc_checksum(__func__);
}

// %2.4

real_t s244(struct args_t * func_args)
{

//    node splitting
//    false dependence cycle breaking

    initialise_arrays(__func__);
    ROI_BEGIN(func_args);

    for (int nl = 0; nl < iterations; nl++) {
        for (int i = 0; i < LEN_1D-1; ++i) {
            a[i] = b[i] + c[i] * d[i];
            b[i] = c[i] + b[i];
            a[i+1] = b[i] + a[i+1] * d[i];
        }
        dummy(a, b, c, d, e, aa, bb, cc, 0.);
    }

    ROI_END(func_args);
    return calc_checksum(__func__);
}

real_t s1244(struct args_t * func_args)
{

//    node splitting
//    cycle with ture and anti dependency

    initialise_arrays(__func__);
    ROI_BEGIN(func_args);

    for (int nl = 0; nl < iterations; nl++) {
        for (int i = 0; i < LEN_1D-1; i++) {
            a[i] = b[i] + c[i] * c[i] + b[i]*b[i] + c[i];
            d[i] = a[i] + a[i+1];
        }
        dummy(a, b, c, d, e, aa, bb, cc, 0.);
    }

    ROI_END(func_args);
    return calc_checksum(__func__);
}

real_t s2244(struct args_t * func_args)
{

//    node splitting
//    cycle with ture and anti dependency

    initialise_arrays(__func__);
    ROI_BEGIN(func_args);

    for (int nl = 0; nl < iterations; nl++) {
        for (int i = 0; i < LEN_1D-1; i++) {
            a[i+1] = b[i] + e[i];
            a[i] = b[i] + c[i];
        }
        dummy(a, b, c, d, e, aa, bb, cc, 0.);
    }

    ROI_END(func_args);
    return calc_checksum(__func__);
}

// %2.5

real_t s251(struct args_t * func_args)
{

//    scalar and array expansion
//    scalar expansion

    initialise_arrays(__func__);
    ROI_BEGIN(func_args);

    real_t s;
    for (int nl = 0; nl < 4*iterations; nl++) {
        for (int i = 0; i < LEN_1D; i++) {
            s = b[i] + c[i] * d[i];
            a[i] = s * s;
        }
        dummy(a, b, c, d, e, aa, bb, cc, 0.);
    }

    ROI_END(func_args);
    return calc_checksum(__func__);
}

real_t s1251(struct args_t * func_args)
{

//    scalar and array expansion
//    scalar expansion

    initialise_arrays(__func__);
    ROI_BEGIN(func_args);

    real_t s;
    for (int nl = 0; nl < 4*iterations; nl++) {
        for (int i = 0; i < LEN_1D; i++) {
            s = b[i]+c[i];
            b[i] = a[i]+d[i];
            a[i] = s*e[i];
        }
        dummy(a, b, c, d, e, aa, bb, cc, 0.);
    }

    ROI_END(func_args);
    return calc_checksum(__func__);
}

real_t s2251(struct args_t * func_args)
{

//    scalar and array expansion
//    scalar expansion

    initialise_arrays(__func__);
    ROI_BEGIN(func_args);

    for (int nl = 0; nl < iterations; nl++) {
        real_t s = (real_t)0.0;
        for (int i = 0; i < LEN_1D; i++) {
            a[i] = s*e[i];
            s = b[i]+c[i];
            b[i] = a[i]+d[i];
        }
        dummy(a, b, c, d, e, aa, bb, cc, 0.);
    }

    ROI_END(func_args);
    return calc_checksum(__func__);
}

real_t s3251(struct args_t * func_args)
{

//    scalar and array expansion
//    scalar expansion

    initialise_arrays(__func__);
    ROI_BEGIN(func_args);

    for (int nl = 0; nl < iterations; nl++) {
        for (int i = 0; i < LEN_1D-1; i++){
            a[i+1] = b[i]+c[i];
            b[i]   = c[i]*e[i];
            d[i]   = a[i]*e[i];
        }
        dummy(a, b, c, d, e, aa, bb, cc, 0.);
    }

    ROI_END(func_args);
    return calc_checksum(__func__);
}

// %2.5

real_t s252(struct args_t * func_args)
{

//    scalar and array expansion
//    loop with ambiguous scalar temporary

    initialise_arrays(__func__);
    ROI_BEGIN(func_args);

    real_t t, s;
    for (int nl = 0; nl < iterations; nl++) {
        t = (real_t) 0.;
        for (int i = 0; i < LEN_1D; i++) {
            s = b[i] * c[i];
            a[i] = s + t;
            t = s;
        }
        dummy(a, b, c, d, e, aa, bb, cc, 0.);
    }

    ROI_END(func_args);
    return calc_checksum(__func__);
}

// %2.5

real_t s253(struct args_t * func_args)
{

//    scalar and array expansion
//    scalar expansio assigned under if

    initialise_arrays(__func__);
    ROI_BEGIN(func_args);

    real_t s;
    for (int nl = 0; nl < iterations; nl++) {
        for (int i = 0; i < LEN_1D; i++) {
            if (a[i] > b[i]) {
                s = a[i] - b[i] * d[i];
                c[i] += s;
                a[i] = s;
            }
        }
        dummy(a, b, c, d, e, aa, bb, cc, 0.);
    }

    ROI_END(func_args);
    return calc_checksum(__func__);
}

// %2.5

real_t s254(struct args_t * func_args)
{

//    scalar and array expansion
//    carry around variable

    initialise_arrays(__func__);
    ROI_BEGIN(func_args);

    real_t x;
    for (int nl = 0; nl < 4*iterations; nl++) {
        x = b[LEN_1D-1];
        for (int i = 0; i < LEN_1D; i++) {
            a[i] = (b[i] + x) * (real_t).5;
            x = b[i];
        }
        dummy(a, b, c, d, e, aa, bb, cc, 0.);
    }

    ROI_END(func_args);
    return calc_checksum(__func__);
}

// %2.5

real_t s255(struct args_t * func_args)
{

//    scalar and array expansion
//    carry around variables, 2 levels

    initialise_arrays(__func__);
    ROI_BEGIN(func_args);

    real_t x, y;
    for (int nl = 0; nl < iterations; nl++) {
        x = b[LEN_1D-1];
        y = b[LEN_1D-2];
        for (int i = 0; i < LEN_1D; i++) {
            a[i] = (b[i] + x + y) * (real_t).333;
            y = x;
            x = b[i];
        }
        dummy(a, b, c, d, e, aa, bb, cc, 0.);
    }

    ROI_END(func_args);
    return calc_checksum(__func__);
}

// %2.5

real_t s256(struct args_t * func_args)
{

//    scalar and array expansion
//    array expansion

    initialise_arrays(__func__);
    ROI_BEGIN(func_args);

    for (int nl = 0; nl < 10*(iterations/LEN_2D); nl++) {
        for (int i = 0; i < LEN_2D; i++) {
            for (int j = 1; j < LEN_2D; j++) {
                a[j] = (real_t)1.0 - a[j - 1];
                aa[j][i] = a[j] + bb[j][i]*d[j];
            }
        }
        dummy(a, b, c, d, e, aa, bb, cc, 0.);
    }

    ROI_END(func_args);
    return calc_checksum(__func__);
}

// %2.5

real_t s257(struct args_t * func_args)
{

//    scalar and array expansion
//    array expansion

    initialise_arrays(__func__);
    ROI_BEGIN(func_args);

    for (int nl = 0; nl < 10*(iterations/LEN_2D); nl++) {
        for (int i = 1; i < LEN_2D; i++) {
            for (int j = 0; j < LEN_2D; j++) {
                a[i] = aa[j][i] - a[i-1];
                aa[j][i] = a[i] + bb[j][i];
            }
        }
        dummy(a, b, c, d, e, aa, bb, cc, 0.);
    }

    ROI_END(func_args);
    return calc_checksum(__func__);
}

real_t s258(struct args_t * func_args)
{

//    scalar and array expansion
//    wrap-around scalar under an if

    initialise_arrays(__func__);
    ROI_BEGIN(func_args);

    real_t s;
    for (int nl = 0; nl < iterations; nl++) {
        s = 0.;
        for (int i = 0; i < LEN_2D; ++i) {
            if (a[i] > 0.) {
                s = d[i] * d[i];
            }
            b[i] = s * c[i] + d[i];
            e[i] = (s + (real_t)1.) * aa[0][i];
        }
        dummy(a, b, c, d, e, aa, bb, cc, 0.);
    }

    ROI_END(func_args);
    return calc_checksum(__func__);
}

// %2.7

real_t s261(struct args_t * func_args)
{

//    scalar and array expansion
//    wrap-around scalar under an if

    initialise_arrays(__func__);
    ROI_BEGIN(func_args);

    real_t t;
    for (int nl = 0; nl < iterations; nl++) {
        for (int i = 1; i < LEN_1D; ++i) {
            t = a[i] + b[i];
            a[i] = t + c[i-1];
            t = c[i] * d[i];
            c[i] = t;
        }
        dummy(a, b, c, d, e, aa, bb, cc, 0.);
    }

    ROI_END(func_args);
    return calc_checksum(__func__);
}

real_t s271(struct args_t * func_args)
{

//    control flow
//    loop with singularity handling

    initialise_arrays(__func__);
    ROI_BEGIN(func_args);

    for (int nl = 0; nl < 4*iterations; nl++) {
        for (int i = 0; i < LEN_1D; i++) {
            if (b[i] > (real_t)0.) {
                a[i] += b[i] * c[i];
            }
        }
        dummy(a, b, c, d, e, aa, bb, cc, 0.);
    }

    ROI_END(func_args);
    return calc_checksum(__func__);
}

// %2.7

//int s272(real_t t)
real_t s272(struct args_t * func_args)
{

//    control flow
//    loop with independent conditional

    int t = *(int*)func_args->arg_info;

    initialise_arrays(__func__);
    ROI_BEGIN(func_args);

    for (int nl = 0; nl < iterations; nl++) {
        for (int i = 0; i < LEN_1D; i++) {
            if (e[i] >= t) {
                a[i] += c[i] * d[i];
                b[i] += c[i] * c[i];
            }
        }
        dummy(a, b, c, d, e, aa, bb, cc, 0.);
    }

    ROI_END(func_args);
    return calc_checksum(__func__);
}

// %2.7

real_t s273(struct args_t * func_args)
{

//    control flow
//    simple loop with dependent conditional

    initialise_arrays(__func__);
    ROI_BEGIN(func_args);

    for (int nl = 0; nl < iterations; nl++) {
        for (int i = 0; i < LEN_1D; i++) {
            a[i] += d[i] * e[i];
            if (a[i] < (real_t)0.)
                b[i] += d[i] * e[i];
            c[i] += a[i] * d[i];
        }
        dummy(a, b, c, d, e, aa, bb, cc, 0.);
    }

    ROI_END(func_args);
    return calc_checksum(__func__);
}

// %2.7

real_t s274(struct args_t * func_args)
{

//    control flow
//    complex loop with dependent conditional

    initialise_arrays(__func__);
    ROI_BEGIN(func_args);

    for (int nl = 0; nl < iterations; nl++) {
        for (int i = 0; i < LEN_1D; i++) {
            a[i] = c[i] + e[i] * d[i];
            if (a[i] > (real_t)0.) {
                b[i] = a[i] + b[i];
            } else {
                a[i] = d[i] * e[i];
            }
        }
        dummy(a, b, c, d, e, aa, bb, cc, 0.);
    }

    ROI_END(func_args);
    return calc_checksum(__func__);
}

// %2.7

real_t s275(struct args_t * func_args)
{

//    control flow
//    if around inner loop, interchanging needed

    initialise_arrays(__func__);
    ROI_BEGIN(func_args);

    for (int nl = 0; nl < 10*(iterations/LEN_2D); nl++) {
        for (int i = 0; i < LEN_2D; i++) {
            if (aa[0][i] > (real_t)0.) {
                for (int j = 1; j < LEN_2D; j++) {
                    aa[j][i] = aa[j-1][i] + bb[j][i] * cc[j][i];
                }
            }
        }
        dummy(a, b, c, d, e, aa, bb, cc, 0.);
    }

    ROI_END(func_args);
    return calc_checksum(__func__);
}

real_t s2275(struct args_t * func_args)
{

//    loop distribution is needed to be able to interchange

    initialise_arrays(__func__);
    ROI_BEGIN(func_args);

    for (int nl = 0; nl < 100*(iterations/LEN_2D); nl++) {
        for (int i = 0; i < LEN_2D; i++) {
            for (int j = 0; j < LEN_2D; j++) {
                aa[j][i] = aa[j][i] + bb[j][i] * cc[j][i];
            }
            a[i] = b[i] + c[i] * d[i];
        }
        dummy(a, b, c, d, e, aa, bb, cc, 0.);
    }

    ROI_END(func_args);
    return calc_checksum(__func__);
}

// %2.7

real_t s276(struct args_t * func_args)
{

//    control flow
//    if test using loop index

    initialise_arrays(__func__);
    ROI_BEGIN(func_args);

    int mid = (LEN_1D/2);
    for (int nl = 0; nl < 4*iterations; nl++) {
        for (int i = 0; i < LEN_1D; i++) {
            if (i+1 < mid) {
                a[i] += b[i] * c[i];
            } else {
                a[i] += b[i] * d[i];
            }
        }
        dummy(a, b, c, d, e, aa, bb, cc, 0.);
    }

    ROI_END(func_args);
    return calc_checksum(__func__);
}

// %2.7
real_t s277(struct args_t * func_args)
{

//    control flow
//    test for dependences arising from guard variable computation.

    initialise_arrays(__func__);
    ROI_BEGIN(func_args);

    for (int nl = 0; nl < iterations; nl++) {
        for (int i = 0; i < LEN_1D-1; i++) {
                if (a[i] >= (real_t)0.) {
                    goto L20;
                }
                if (b[i] >= (real_t)0.) {
                    goto L30;
                }
                a[i] += c[i] * d[i];
L30:
                b[i+1] = c[i] + d[i] * e[i];
L20:
;
        }
        dummy(a, b, c, d, e, aa, bb, cc, 0.);
    }

    ROI_END(func_args);
    return calc_checksum(__func__);
}

// %2.7

real_t s278(struct args_t * func_args)
{

//    control flow
//    if/goto to block if-then-else

    initialise_arrays(__func__);
    ROI_BEGIN(func_args);

    for (int nl = 0; nl < iterations; nl++) {
        for (int i = 0; i < LEN_1D; i++) {
            if (a[i] > (real_t)0.) {
                goto L20;
            }
            b[i] = -b[i] + d[i] * e[i];
            goto L30;
L20:
            c[i] = -c[i] + d[i] * e[i];
L30:
            a[i] = b[i] + c[i] * d[i];
        }
        dummy(a, b, c, d, e, aa, bb, cc, 0.);
    }

    ROI_END(func_args);
    return calc_checksum(__func__);
}

// %2.7

real_t s279(struct args_t * func_args)
{

//    control flow
//    vector if/gotos

    initialise_arrays(__func__);
    ROI_BEGIN(func_args);

    for (int nl = 0; nl < iterations/2; nl++) {
        for (int i = 0; i < LEN_1D; i++) {
            if (a[i] > (real_t)0.) {
                goto L20;
            }
            b[i] = -b[i] + d[i] * d[i];
            if (b[i] <= a[i]) {
                goto L30;
            }
            c[i] += d[i] * e[i];
            goto L30;
L20:
            c[i] = -c[i] + e[i] * e[i];
L30:
            a[i] = b[i] + c[i] * d[i];
        }
        dummy(a, b, c, d, e, aa, bb, cc, 0.);
    }

    ROI_END(func_args);
    return calc_checksum(__func__);
}

real_t s1279(struct args_t * func_args)
{

//    control flow
//    vector if/gotos

    initialise_arrays(__func__);
    ROI_BEGIN(func_args);

    for (int nl = 0; nl < iterations; nl++) {
        for (int i = 0; i < LEN_1D; i++) {
            if (a[i] < (real_t)0.) {
                if (b[i] > a[i]) {
                    c[i] += d[i] * e[i];
                }
            }
        }
        dummy(a, b, c, d, e, aa, bb, cc, 0.);
    }

    ROI_END(func_args);
    return calc_checksum(__func__);
}

// %2.7

//int s2710( real_t x)
real_t s2710(struct args_t * func_args)
{

//    control flow
//    scalar and vector ifs

    int x = *(int*)func_args->arg_info;

    initialise_arrays(__func__);
    ROI_BEGIN(func_args);

    for (int nl = 0; nl < iterations/2; nl++) {
        for (int i = 0; i < LEN_1D; i++) {
            if (a[i] > b[i]) {
                a[i] += b[i] * d[i];
                if (LEN_1D > 10) {
                    c[i] += d[i] * d[i];
                } else {
                    c[i] = d[i] * e[i] + (real_t)1.;
                }
            } else {
                b[i] = a[i] + e[i] * e[i];
                if (x > (real_t)0.) {
                    c[i] = a[i] + d[i] * d[i];
                } else {
                    c[i] += e[i] * e[i];
                }
            }
        }
        dummy(a, b, c, d, e, aa, bb, cc, 0.);
    }

    ROI_END(func_args);
    return calc_checksum(__func__);
}

// %2.7

real_t s2711(struct args_t * func_args)
{

//    control flow
//    semantic if removal

    initialise_arrays(__func__);
    ROI_BEGIN(func_args);

    for (int nl = 0; nl < 4*iterations; nl++) {
        for (int i = 0; i < LEN_1D; i++) {
            if (b[i] != (real_t)0.0) {
                a[i] += b[i] * c[i];
            }
        }
        dummy(a, b, c, d, e, aa, bb, cc, 0.);
    }

    ROI_END(func_args);
    return calc_checksum(__func__);
}

// %2.7

real_t s2712(struct args_t * func_args)
{

//    control flow
//    if to elemental min

    initialise_arrays(__func__);
    ROI_BEGIN(func_args);

    for (int nl = 0; nl < 4*iterations; nl++) {
        for (int i = 0; i < LEN_1D; i++) {
            if (a[i] > b[i]) {
                a[i] += b[i] * c[i];
            }
        }
        dummy(a, b, c, d, e, aa, bb, cc, 0.);
    }

    ROI_END(func_args);
    return calc_checksum(__func__);
}

// %2.8

real_t s281(struct args_t * func_args)
{

//    crossing thresholds
//    index set splitting
//    reverse data access

    initialise_arrays(__func__);
    ROI_BEGIN(func_args);

    real_t x;
    for (int nl = 0; nl < iterations; nl++) {
        for (int i = 0; i < LEN_1D; i++) {
            x = a[LEN_1D-i-1] + b[i] * c[i];
            a[i] = x-(real_t)1.0;
            b[i] = x;
        }
        dummy(a, b, c, d, e, aa, bb, cc, 0.);
    }

    ROI_END(func_args);
    return calc_checksum(__func__);
}

real_t s1281(struct args_t * func_args)
{

//    crossing thresholds
//    index set splitting
//    reverse data access

    initialise_arrays(__func__);
    ROI_BEGIN(func_args);

    real_t x;
    for (int nl = 0; nl < 4*iterations; nl++) {
        for (int i = 0; i < LEN_1D; i++) {
            x = b[i]*c[i] + a[i]*d[i] + e[i];
            a[i] = x-(real_t)1.0;
            b[i] = x;
        }
        dummy(a, b, c, d, e, aa, bb, cc, 0.);
    }

    ROI_END(func_args);
    return calc_checksum(__func__);
}

// %2.9

real_t s291(struct args_t * func_args)
{

//    loop peeling
//    wrap around variable, 1 level

    initialise_arrays(__func__);
    ROI_BEGIN(func_args);

    int im1;
    for (int nl = 0; nl < 2*iterations; nl++) {
        im1 = LEN_1D-1;
        for (int i = 0; i < LEN_1D; i++) {
            a[i] = (b[i] + b[im1]) * (real_t).5;
            im1 = i;
        }
        dummy(a, b, c, d, e, aa, bb, cc, 0.);
    }

    ROI_END(func_args);
    return calc_checksum(__func__);
}

// %2.9

real_t s292(struct args_t * func_args)
{

//    loop peeling
//    wrap around variable, 2 levels
//    similar to S291

    initialise_arrays(__func__);
    ROI_BEGIN(func_args);

    int im1, im2;
    for (int nl = 0; nl < iterations; nl++) {
        im1 = LEN_1D-1;
        im2 = LEN_1D-2;
        for (int i = 0; i < LEN_1D; i++) {
            a[i] = (b[i] + b[im1] + b[im2]) * (real_t).333;
            im2 = im1;
            im1 = i;
        }
        dummy(a, b, c, d, e, aa, bb, cc, 0.);
    }

    ROI_END(func_args);
    return calc_checksum(__func__);
}

// %2.9

real_t s293(struct args_t * func_args)
{

//    loop peeling
//    a(i)=a(0) with actual dependence cycle, loop is vectorizable

    initialise_arrays(__func__);
    ROI_BEGIN(func_args);

    for (int nl = 0; nl < 4*iterations; nl++) {
        for (int i = 0; i < LEN_1D; i++) {
            a[i] = a[0];
        }
        dummy(a, b, c, d, e, aa, bb, cc, 0.);
    }

    ROI_END(func_args);
    return calc_checksum(__func__);
}

// %2.10

real_t s2101(struct args_t * func_args)
{

//    diagonals
//    main diagonal calculation
//    jump in data access

    initialise_arrays(__func__);
    ROI_BEGIN(func_args);

    for (int nl = 0; nl < 10*iterations; nl++) {
        for (int i = 0; i < LEN_2D; i++) {
            aa[i][i] += bb[i][i] * cc[i][i];
        }
        dummy(a, b, c, d, e, aa, bb, cc, 0.);
    }

    ROI_END(func_args);
    return calc_checksum(__func__);
}

// %2.12

real_t s2102(struct args_t * func_args)
{

//    diagonals
//    identity matrix, best results vectorize both inner and outer loops

    initialise_arrays(__func__);
    ROI_BEGIN(func_args);

    for (int nl = 0; nl < 100*(iterations/LEN_2D); nl++) {
        for (int i = 0; i < LEN_2D; i++) {
            for (int j = 0; j < LEN_2D; j++) {
                aa[j][i] = (real_t)0.;
            }
            aa[i][i] = (real_t)1.;
        }
        dummy(a, b, c, d, e, aa, bb, cc, 0.);
    }

    ROI_END(func_args);
    return calc_checksum(__func__);
}

// %2.11

real_t s2111(struct args_t * func_args)
{

//    wavefronts, it will make jump in data access

    initialise_arrays(__func__);
    ROI_BEGIN(func_args);

    for (int nl = 0; nl < 100*(iterations/(LEN_2D)); nl++) {
        for (int j = 1; j < LEN_2D; j++) {
            for (int i = 1; i < LEN_2D; i++) {
                aa[j][i] = (aa[j][i-1] + aa[j-1][i])/1.9;
            }
        }
        dummy(a, b, c, d, e, aa, bb, cc, 0.);
    }

    ROI_END(func_args);
    return calc_checksum(__func__);
}

// **********************************************************
//                                *
//            IDIOM RECOGNITION            *
//                                *
// **********************************************************

// %3.1

real_t s311(struct args_t * func_args)
{

//    reductions
//    sum reduction

    initialise_arrays(__func__);
    ROI_BEGIN(func_args);

    real_t sum;
    for (int nl = 0; nl < iterations*10; nl++) {
        sum = (real_t)0.;
        for (int i = 0; i < LEN_1D; i++) {
            sum += a[i];
        }
        dummy(a, b, c, d, e, aa, bb, cc, sum);
    }

    ROI_END(func_args);
    return calc_checksum(__func__);
}

real_t test(real_t* A){
  real_t s = (real_t)0.0;
  for (int i = 0; i < 4; i++)
    s += A[i];
  return s;
}

real_t s31111(struct args_t * func_args)
{

//    reductions
//    sum reduction

    initialise_arrays(__func__);
    ROI_BEGIN(func_args);

    real_t sum;
    for (int nl = 0; nl < 2000*iterations; nl++) {
        sum = (real_t)0.;
        sum += test(a);
        sum += test(&a[4]);
        sum += test(&a[8]);
        sum += test(&a[12]);
        sum += test(&a[16]);
        sum += test(&a[20]);
        sum += test(&a[24]);
        sum += test(&a[28]);
        dummy(a, b, c, d, e, aa, bb, cc, sum);
    }

    ROI_END(func_args);
    return calc_checksum(__func__);
}

// %3.1

real_t s312(struct args_t * func_args)
{

//    reductions
//    product reduction

    initialise_arrays(__func__);
    ROI_BEGIN(func_args);

    real_t prod;
    for (int nl = 0; nl < 10*iterations; nl++) {
        prod = (real_t)1.;
        for (int i = 0; i < LEN_1D; i++) {
            prod *= a[i];
        }
        dummy(a, b, c, d, e, aa, bb, cc, prod);
    }

    ROI_END(func_args);
    return prod;
}

// %3.1
real_t s313(struct args_t * func_args)
{

//    reductions
//    dot product

    initialise_arrays(__func__);
    ROI_BEGIN(func_args);

    real_t dot;
    for (int nl = 0; nl < iterations*5; nl++) {
        dot = (real_t)0.;
        for (int i = 0; i < LEN_1D; i++) {
            dot += a[i] * b[i];
        }
        dummy(a, b, c, d, e, aa, bb, cc, dot);
    }

    ROI_END(func_args);
    return dot;
}

// %3.1

real_t s314(struct args_t * func_args)
{

//    reductions
//    if to max reduction

    initialise_arrays(__func__);
    ROI_BEGIN(func_args);

    real_t x;
    for (int nl = 0; nl < iterations*5; nl++) {
        x = a[0];
        for (int i = 0; i < LEN_1D; i++) {
            if (a[i] > x) {
                x = a[i];
            }
        }
        dummy(a, b, c, d, e, aa, bb, cc, x);
    }

    ROI_END(func_args);
    return x;
}

// %3.1

real_t s315(struct args_t * func_args)
{

//    reductions
//    if to max with index reductio 1 dimension

    initialise_arrays(__func__);
    ROI_BEGIN(func_args);

    for (int i = 0; i < LEN_1D; i++)
        a[i] = (i * 7) % LEN_1D;

    real_t x, chksum;
    int index;
    for (int nl = 0; nl < iterations; nl++) {
        x = a[0];
        index = 0;
        for (int i = 0; i < LEN_1D; ++i) {
            if (a[i] > x) {
                x = a[i];
                index = i;
            }
        }
        chksum = x + (real_t) index;
        dummy(a, b, c, d, e, aa, bb, cc, chksum);
    }

    ROI_END(func_args);
    return index + x + 1;
}

// %3.1

real_t s316(struct args_t * func_args)
{

//    reductions
//    if to min reduction

    initialise_arrays(__func__);
    ROI_BEGIN(func_args);

    real_t x;
    for (int nl = 0; nl < iterations*5; nl++) {
        x = a[0];
        for (int i = 1; i < LEN_1D; ++i) {
            if (a[i] < x) {
                x = a[i];
            }
        }
        dummy(a, b, c, d, e, aa, bb, cc, x);
    }

    ROI_END(func_args);
    return x;
}
// %3.1

real_t s317(struct args_t * func_args)
{

//    reductions
//    product reductio vectorize with
//    1. scalar expansion of factor, and product reduction
//    2. closed form solution: q = factor**n

    initialise_arrays(__func__);
    ROI_BEGIN(func_args);

    real_t q;
    for (int nl = 0; nl < 5*iterations; nl++) {
        q = (real_t)1.;
        for (int i = 0; i < LEN_1D/2; i++) {
            q *= (real_t).99;
        }
        dummy(a, b, c, d, e, aa, bb, cc, q);
    }

    ROI_END(func_args);
    return q;
}

// %3.1

//int s318( int inc)
real_t s318(struct args_t * func_args)
{

//    reductions
//    isamax, max absolute value, increments not equal to 1

    int inc = *(int*)func_args->arg_info;

    initialise_arrays(__func__);
    ROI_BEGIN(func_args);

    int k, index;
    real_t max, chksum;
    for (int nl = 0; nl < iterations/2; nl++) {
        k = 0;
        index = 0;
        max = ABS(a[0]);
        k += inc;
        for (int i = 1; i < LEN_1D; i++) {
            if (ABS(a[k]) <= max) {
                goto L5;
            }
            index = i;
            max = ABS(a[k]);
L5:
            k += inc;
        }
        chksum = max + (real_t) index;
        dummy(a, b, c, d, e, aa, bb, cc, chksum);
    }

    ROI_END(func_args);
    return max + index + 1;
}

// %3.1

real_t s319(struct args_t * func_args)
{

//    reductions
//    coupled reductions

    initialise_arrays(__func__);
    ROI_BEGIN(func_args);

    real_t sum;
    for (int nl = 0; nl < 2*iterations; nl++) {
        sum = 0.;
        for (int i = 0; i < LEN_1D; i++) {
            a[i] = c[i] + d[i];
            sum += a[i];
            b[i] = c[i] + e[i];
            sum += b[i];
        }
        dummy(a, b, c, d, e, aa, bb, cc, sum);
    }

    ROI_END(func_args);
    return sum;
}

// %3.1

real_t s3110(struct args_t * func_args)
{

//    reductions
//    if to max with index reductio 2 dimensions
//    similar to S315

    initialise_arrays(__func__);
    ROI_BEGIN(func_args);

    int xindex, yindex;
    real_t max, chksum;
    for (int nl = 0; nl < 100*(iterations/(LEN_2D)); nl++) {
        max = aa[(0)][0];
        xindex = 0;
        yindex = 0;
        for (int i = 0; i < LEN_2D; i++) {
            for (int j = 0; j < LEN_2D; j++) {
                if (aa[i][j] > max) {
                    max = aa[i][j];
                    xindex = i;
                    yindex = j;
                }
            }
        }
        chksum = max + (real_t) xindex + (real_t) yindex;
        dummy(a, b, c, d, e, aa, bb, cc, chksum);
    }

    ROI_END(func_args);
    return max + xindex+1 + yindex+1;
}

real_t s13110(struct args_t * func_args)
{

//    reductions
//    if to max with index reductio 2 dimensions

    initialise_arrays(__func__);
    ROI_BEGIN(func_args);

    int xindex, yindex;
    real_t max, chksum;
    for (int nl = 0; nl < 100*(iterations/(LEN_2D)); nl++) {
        max = aa[(0)][0];
        xindex = 0;
        yindex = 0;
        for (int i = 0; i < LEN_2D; i++) {
            for (int j = 0; j < LEN_2D; j++) {
                if (aa[i][j] > max) {
                    max = aa[i][j];
                    xindex = i;
                    yindex = j;
                }
            }
        }
        chksum = max + (real_t) xindex + (real_t) yindex;
        dummy(a, b, c, d, e, aa, bb, cc, chksum);
    }

    ROI_END(func_args);
    return max + xindex+1 + yindex+1;
}

// %3.1

real_t s3111(struct args_t * func_args)
{

//    reductions
//    conditional sum reduction

    initialise_arrays(__func__);
    ROI_BEGIN(func_args);

    real_t sum;
    for (int nl = 0; nl < iterations/2; nl++) {
        sum = 0.;
        for (int i = 0; i < LEN_1D; i++) {
            if (a[i] > (real_t)0.) {
                sum += a[i];
            }
        }
        dummy(a, b, c, d, e, aa, bb, cc, sum);
    }

    ROI_END(func_args);
    return sum;
}

// %3.1

real_t s3112(struct args_t * func_args)
{

//    reductions
//    sum reduction saving running sums

    initialise_arrays(__func__);
    ROI_BEGIN(func_args);

    real_t sum;
    for (int nl = 0; nl < iterations; nl++) {
        sum = (real_t)0.0;
        for (int i = 0; i < LEN_1D; i++) {
            sum += a[i];
            b[i] = sum;
        }
        dummy(a, b, c, d, e, aa, bb, cc, sum);
    }

    ROI_END(func_args);
    return sum;
}

// %3.1

real_t s3113(struct args_t * func_args)
{

//    reductions
//    maximum of absolute value

    initialise_arrays(__func__);
    ROI_BEGIN(func_args);

    real_t max;
    for (int nl = 0; nl < iterations*4; nl++) {
        max = ABS(a[0]);
        for (int i = 0; i < LEN_1D; i++) {
            if ((ABS(a[i])) > max) {
                max = ABS(a[i]);
            }
        }
        dummy(a, b, c, d, e, aa, bb, cc, max);
    }

    ROI_END(func_args);
    return max;
}

// %3.2

real_t s321(struct args_t * func_args)
{

//    recurrences
//    first order linear recurrence

    initialise_arrays(__func__);
    ROI_BEGIN(func_args);

    for (int nl = 0; nl < iterations; nl++) {
        for (int i = 1; i < LEN_1D; i++) {
            a[i] += a[i-1] * b[i];
        }
        dummy(a, b, c, d, e, aa, bb, cc, 0.);
    }

    ROI_END(func_args);
    return calc_checksum(__func__);
}

// %3.2

real_t s322(struct args_t * func_args)
{

//    recurrences
//    second order linear recurrence

    initialise_arrays(__func__);
    ROI_BEGIN(func_args);

    for (int nl = 0; nl < iterations/2; nl++) {
        for (int i = 2; i < LEN_1D; i++) {
            a[i] = a[i] + a[i - 1] * b[i] + a[i - 2] * c[i];
        }
        dummy(a, b, c, d, e, aa, bb, cc, 0.);
    }

    ROI_END(func_args);
    return calc_checksum(__func__);
}

// %3.2

real_t s323(struct args_t * func_args)
{

//    recurrences
//    coupled recurrence

    initialise_arrays(__func__);
    ROI_BEGIN(func_args);

    for (int nl = 0; nl < iterations/2; nl++) {
        for (int i = 1; i < LEN_1D; i++) {
            a[i] = b[i-1] + c[i] * d[i];
            b[i] = a[i] + c[i] * e[i];
        }
        dummy(a, b, c, d, e, aa, bb, cc, 0.);
    }

    ROI_END(func_args);
    return calc_checksum(__func__);
}

// %3.3

real_t s331(struct args_t * func_args)
{

//    search loops
//    if to last-1

    initialise_arrays(__func__);
    ROI_BEGIN(func_args);

    int j;
    real_t chksum;
    for (int nl = 0; nl < iterations; nl++) {
        j = -1;
        for (int i = 0; i < LEN_1D; i++) {
            if (a[i] < (real_t)0.) {
                j = i;
            }
        }
        chksum = (real_t) j;
        dummy(a, b, c, d, e, aa, bb, cc, chksum);
    }

    ROI_END(func_args);
    return j+1;
}

// %3.3
//int s332( real_t t)
real_t s332(struct args_t * func_args)
{

//    search loops
//    first value greater than threshold

    int t = *(int*)func_args->arg_info;

    initialise_arrays(__func__);
    ROI_BEGIN(func_args);

    int index;
    real_t value;
    real_t chksum;
    for (int nl = 0; nl < iterations; nl++) {
        index = -2;
        value = -1.;
        for (int i = 0; i < LEN_1D; i++) {
            if (a[i] > t) {
                index = i;
                value = a[i];
                goto L20;
            }
        }
L20:
        chksum = value + (real_t) index;
        dummy(a, b, c, d, e, aa, bb, cc, chksum);
    }

    ROI_END(func_args);
    return value;
}

// %3.4

real_t s341(struct args_t * func_args)
{

//    packing
//    pack positive values
//    not vectorizable, value of j in unknown at each iteration

    initialise_arrays(__func__);
    ROI_BEGIN(func_args);

    int j;
    for (int nl = 0; nl < iterations; nl++) {
        j = -1;
        for (int i = 0; i < LEN_1D; i++) {
            if (b[i] > (real_t)0.) {
                j++;
                a[j] = b[i];
            }
        }
        dummy(a, b, c, d, e, aa, bb, cc, 0.);
    }

    ROI_END(func_args);
    return calc_checksum(__func__);
}

// %3.4

real_t s342(struct args_t * func_args)
{

//    packing
//    unpacking
//    not vectorizable, value of j in unknown at each iteration

    initialise_arrays(__func__);
    ROI_BEGIN(func_args);

    int j = 0;
    for (int nl = 0; nl < iterations; nl++) {
        j = -1;
        for (int i = 0; i < LEN_1D; i++) {
            if (a[i] > (real_t)0.) {
                j++;
                a[i] = b[j];
            }
        }
        dummy(a, b, c, d, e, aa, bb, cc, 0.);
    }

    ROI_END(func_args);
    return calc_checksum(__func__);
}

// %3.4

real_t s343(struct args_t * func_args)
{

//    packing
//    pack 2-d array into one dimension
//    not vectorizable, value of k in unknown at each iteration

    initialise_arrays(__func__);
    ROI_BEGIN(func_args);

    int k;
    for (int nl = 0; nl < 10*(iterations/LEN_2D); nl++) {
        k = -1;
        for (int i = 0; i < LEN_2D; i++) {
            for (int j = 0; j < LEN_2D; j++) {
                if (bb[j][i] > (real_t)0.) {
                    k++;
                    flat_2d_array[k] = aa[j][i];
                }
            }
        }
        dummy(a, b, c, d, e, aa, bb, cc, 0.);
    }

    ROI_END(func_args);
    return calc_checksum(__func__);
}

// %3.5

real_t s351(struct args_t * func_args)
{

//    loop rerolling
//    unrolled saxpy

    initialise_arrays(__func__);
    ROI_BEGIN(func_args);

    real_t alpha = c[0];
    for (int nl = 0; nl < 8*iterations; nl++) {
        for (int i = 0; i < LEN_1D; i += 5) {
            a[i] += alpha * b[i];
            a[i + 1] += alpha * b[i + 1];
            a[i + 2] += alpha * b[i + 2];
            a[i + 3] += alpha * b[i + 3];
            a[i + 4] += alpha * b[i + 4];
        }
        dummy(a, b, c, d, e, aa, bb, cc, 0.);
    }

    ROI_END(func_args);
    return calc_checksum(__func__);
}

real_t s1351(struct args_t * func_args)
{

//    induction pointer recognition

    initialise_arrays(__func__);
    ROI_BEGIN(func_args);

    for (int nl = 0; nl < 8*iterations; nl++) {
        real_t* __restrict__ A = a;
        real_t* __restrict__ B = b;
        real_t* __restrict__ C = c;
        for (int i = 0; i < LEN_1D; i++) {
            *A = *B+*C;
            A++;
            B++;
            C++;
        }
        dummy(a, b, c, d, e, aa, bb, cc, 0.);
    }

    ROI_END(func_args);
    return calc_checksum(__func__);
}

// %3.5

real_t s352(struct args_t * func_args)
{

//    loop rerolling
//    unrolled dot product

    initialise_arrays(__func__);
    ROI_BEGIN(func_args);

    real_t dot;
    for (int nl = 0; nl < 8*iterations; nl++) {
        dot = (real_t)0.;
        for (int i = 0; i < LEN_1D; i += 5) {
            dot = dot + a[i] * b[i] + a[i + 1] * b[i + 1] + a[i + 2]
                * b[i + 2] + a[i + 3] * b[i + 3] + a[i + 4] * b[i + 4];
        }
        dummy(a, b, c, d, e, aa, bb, cc, dot);
    }

    ROI_END(func_args);
    return dot;
}

// %3.5

//int s353(int* __restrict__ ip)
real_t s353(struct args_t * func_args)
{

//    loop rerolling
//    unrolled sparse saxpy
//    gather is required

    int * __restrict__ ip = func_args->arg_info;

    initialise_arrays(__func__);
    ROI_BEGIN(func_args);

    real_t alpha = c[0];
    for (int nl = 0; nl < iterations; nl++) {
        /*
        for (int i = 0; i < LEN_1D; i += 5) {
            a[i] += alpha * b[ip[i]];
            a[i + 1] += alpha * b[ip[i + 1]];
            a[i + 2] += alpha * b[ip[i + 2]];
            a[i + 3] += alpha * b[ip[i + 3]];
            a[i + 4] += alpha * b[ip[i + 4]];
        }
        */
        // Non-indexed equivalent. Valid ONLY for the stock ip[] init
        // (common.c: each aligned 5-block holds {i+4, i+2, i, i+3, i+1});
        // wrong if the randomized Fisher-Yates init is enabled. Size-
        // independent: the closed form covers the 5-aligned prefix, and a
        // scalar tail of < 5 iterations (compile-time-constant trip count,
        // so it stays scalar — no vector gathers) goes through ip[]
        // directly; only that tail still reads ip[]. Expected codegen:
        // unit-stride / segment (vlseg5) / strided (vlse) vector memory
        // ops, no vluxei/vsuxei — verify in the .dump.

        int lim = LEN_1D - LEN_1D % 5;
        for (int i = 0; i < lim; i += 5) {
            a[i]     += alpha * b[i + 4];
            a[i + 1] += alpha * b[i + 2];
            a[i + 2] += alpha * b[i];
            a[i + 3] += alpha * b[i + 3];
            a[i + 4] += alpha * b[i + 1];
        }
        for (int i = lim; i < LEN_1D; i++) {
            a[i] += alpha * b[ip[i]];
        }
        
        dummy(a, b, c, d, e, aa, bb, cc, 0.);
    }

    ROI_END(func_args);
    return calc_checksum(__func__);
}

// **********************************************************
//                                *
//             LANGUAGE COMPLETENESS            *
//                                *
// **********************************************************

// %4.1
// %4.2

real_t s421(struct args_t * func_args)
{

//    storage classes and equivalencing
//    equivalence- no overlap

    initialise_arrays(__func__);
    ROI_BEGIN(func_args);

    xx = flat_2d_array;

    for (int nl = 0; nl < 4*iterations; nl++) {
        yy = xx;
        for (int i = 0; i < LEN_1D - 1; i++) {
            xx[i] = yy[i+1] + a[i];
        }
        dummy(a, b, c, d, e, aa, bb, cc, 1.);
    }

    ROI_END(func_args);
    return calc_checksum(__func__);
}

real_t s1421(struct args_t * func_args)
{

//    storage classes and equivalencing
//    equivalence- no overlap

    initialise_arrays(__func__);
    ROI_BEGIN(func_args);

    xx = &b[LEN_1D/2];

    for (int nl = 0; nl < 8*iterations; nl++) {
        for (int i = 0; i < LEN_1D/2; i++) {
            b[i] = xx[i] + a[i];
        }
        dummy(a, b, c, d, e, aa, bb, cc, 1.);
    }

    ROI_END(func_args);
    return calc_checksum(__func__);
}

// %4.2

real_t s422(struct args_t * func_args)
{

//    storage classes and equivalencing
//    common and equivalence statement
//    anti-dependence, threshold of 4

    initialise_arrays(__func__);
    ROI_BEGIN(func_args);

    xx = flat_2d_array + 4;

    for (int nl = 0; nl < 8*iterations; nl++) {
        for (int i = 0; i < LEN_1D; i++) {
            xx[i] = flat_2d_array[i + 8] + a[i];
        }
        dummy(a, b, c, d, e, aa, bb, cc, 0.);
    }

    ROI_END(func_args);
    return calc_checksum(__func__);
}

// %4.2

real_t s423(struct args_t * func_args)
{

//    storage classes and equivalencing
//    common and equivalenced variables - with anti-dependence

    // do this again here
    int vl = 64;
    xx = flat_2d_array + vl;

    initialise_arrays(__func__);
    ROI_BEGIN(func_args);

    for (int nl = 0; nl < 4*iterations; nl++) {
        for (int i = 0; i < LEN_1D - 1; i++) {
            flat_2d_array[i+1] = xx[i] + a[i];
        }
        dummy(a, b, c, d, e, aa, bb, cc, 1.);
    }

    ROI_END(func_args);
    return calc_checksum(__func__);
}

// %4.2

real_t s424(struct args_t * func_args)
{

//    storage classes and equivalencing
//    common and equivalenced variables - overlap
//    vectorizeable in strips of 64 or less

    // do this again here
    int vl = 63;
    xx = flat_2d_array + vl;

    initialise_arrays(__func__);
    ROI_BEGIN(func_args);

    for (int nl = 0; nl < 4*iterations; nl++) {
        for (int i = 0; i < LEN_1D - 1; i++) {
            xx[i+1] = flat_2d_array[i] + a[i];
        }
        dummy(a, b, c, d, e, aa, bb, cc, 1.);
    }

    ROI_END(func_args);
    return calc_checksum(__func__);
}

// %4.3

real_t s431(struct args_t * func_args)
{

//    parameters
//    parameter statement

    int k1=1;
    int k2=2;
    int k=2*k1-k2;

    initialise_arrays(__func__);
    ROI_BEGIN(func_args);

    for (int nl = 0; nl < iterations*10; nl++) {
        for (int i = 0; i < LEN_1D; i++) {
            a[i] = a[i+k] + b[i];
        }
        dummy(a, b, c, d, e, aa, bb, cc, 0.);
    }

    ROI_END(func_args);
    return calc_checksum(__func__);
}

// %4.4

real_t s441(struct args_t * func_args)
{

//    non-logical if's
//    arithmetic if

    initialise_arrays(__func__);
    ROI_BEGIN(func_args);

    for (int nl = 0; nl < iterations; nl++) {
        for (int i = 0; i < LEN_1D; i++) {
            if (d[i] < (real_t)0.) {
                a[i] += b[i] * c[i];
            } else if (d[i] == (real_t)0.) {
                a[i] += b[i] * b[i];
            } else {
                a[i] += c[i] * c[i];
            }
        }
        dummy(a, b, c, d, e, aa, bb, cc, 0.);
    }

    ROI_END(func_args);
    return calc_checksum(__func__);
}

// %4.4

real_t s442(struct args_t * func_args)
{

//    non-logical if's
//    computed goto

    initialise_arrays(__func__);
    ROI_BEGIN(func_args);

    for (int nl = 0; nl < iterations/2; nl++) {
        for (int i = 0; i < LEN_1D; i++) {
            switch (indx[i]) {
                case 1:  goto L15;
                case 2:  goto L20;
                case 3:  goto L30;
                case 4:  goto L40;
            }
L15:
            a[i] += b[i] * b[i];
            goto L50;
L20:
            a[i] += c[i] * c[i];
            goto L50;
L30:
            a[i] += d[i] * d[i];
            goto L50;
L40:
            a[i] += e[i] * e[i];
L50:
            ;
        }
        dummy(a, b, c, d, e, aa, bb, cc, 0.);
    }

    ROI_END(func_args);
    return calc_checksum(__func__);
}

// %4.4

real_t s443(struct args_t * func_args)
{

//    non-logical if's
//    arithmetic if

    initialise_arrays(__func__);
    ROI_BEGIN(func_args);

    for (int nl = 0; nl < 2*iterations; nl++) {
        for (int i = 0; i < LEN_1D; i++) {
            if (d[i] <= (real_t)0.) {
                goto L20;
            } else {
                goto L30;
            }
L20:
            a[i] += b[i] * c[i];
            goto L50;
L30:
            a[i] += b[i] * b[i];
L50:
            ;
        }
        dummy(a, b, c, d, e, aa, bb, cc, 0.);
    }

    ROI_END(func_args);
    return calc_checksum(__func__);
}

// %4.5

real_t s451(struct args_t * func_args)
{

//    intrinsic functions
//    intrinsics

    initialise_arrays(__func__);
    ROI_BEGIN(func_args);

    for (int nl = 0; nl < iterations/5; nl++) {
        for (int i = 0; i < LEN_1D; i++) {
            a[i] = sinf(b[i]) + cosf(c[i]);
        }
        dummy(a, b, c, d, e, aa, bb, cc, 0.);
    }

    ROI_END(func_args);
    return calc_checksum(__func__);
}

// %4.5

real_t s452(struct args_t * func_args)
{

//    intrinsic functions
//    seq function

    initialise_arrays(__func__);
    ROI_BEGIN(func_args);

    for (int nl = 0; nl < 4*iterations; nl++) {
        for (int i = 0; i < LEN_1D; i++) {
            a[i] = b[i] + c[i] * (real_t) (i+1);
        }
        dummy(a, b, c, d, e, aa, bb, cc, 0.);
    }

    ROI_END(func_args);
    return calc_checksum(__func__);
}

// %4.5

real_t s453(struct args_t * func_args)
{

//    induction varibale recognition

    real_t s;

    initialise_arrays(__func__);
    ROI_BEGIN(func_args);

    for (int nl = 0; nl < iterations*2; nl++) {
        s = 0.;
        for (int i = 0; i < LEN_1D; i++) {
            s += (real_t)2.;
            a[i] = s * b[i];
        }
        dummy(a, b, c, d, e, aa, bb, cc, 0.);
    }

    ROI_END(func_args);
    return calc_checksum(__func__);
}

// %4.7

int s471s(void)
{
// --  dummy subroutine call made in s471
    return 0;
}

real_t s471(struct args_t * func_args){

//    call statements

    int m = LEN_1D;

    initialise_arrays(__func__);
    ROI_BEGIN(func_args);

    for (int nl = 0; nl < iterations/2; nl++) {
        for (int i = 0; i < m; i++) {
            x[i] = b[i] + d[i] * d[i];
            s471s();
            b[i] = c[i] + d[i] * e[i];
        }
        dummy(a, b, c, d, e, aa, bb, cc, 0.);
    }

    ROI_END(func_args);
    return calc_checksum(__func__);
}

// %4.8

real_t s481(struct args_t * func_args)
{

//    non-local goto's
//    stop statement

    initialise_arrays(__func__);
    ROI_BEGIN(func_args);

    for (int nl = 0; nl < iterations; nl++) {
        for (int i = 0; i < LEN_1D; i++) {
            if (d[i] < (real_t)0.) {
                exit (0);
            }
            a[i] += b[i] * c[i];
        }
        dummy(a, b, c, d, e, aa, bb, cc, 0.);
    }

    ROI_END(func_args);
    return calc_checksum(__func__);
}

// %4.8

// %4.8
real_t s482(struct args_t * func_args)
{

//    non-local goto's
//    other loop exit with code before exit

    initialise_arrays(__func__);
    ROI_BEGIN(func_args);

    for (int nl = 0; nl < iterations; nl++) {
        for (int i = 0; i < LEN_1D; i++) {
            a[i] += b[i] * c[i];
            if (c[i] > b[i]) break;
        }
        dummy(a, b, c, d, e, aa, bb, cc, 0.);
    }

    ROI_END(func_args);
    return calc_checksum(__func__);
}

// %4.9

//int s491(int* __restrict__ ip)
real_t s491(struct args_t * func_args)
{

//    vector semantics
//    indirect addressing on lhs, store in sequence
//    scatter is required

    int * __restrict__ ip = func_args->arg_info;

    initialise_arrays(__func__);
    ROI_BEGIN(func_args);

    for (int nl = 0; nl < iterations; nl++) {
        /*
        for (int i = 0; i < LEN_1D; i++) {
            a[ip[i]] = b[i] + c[i] * d[i];
        }
        */
        // Non-indexed equivalent (stock block-5 ip[] only, see s353 note;
        // 5-aligned prefix + scalar ip[] tail makes it LEN_1D-agnostic).
        // Scatter rewritten through the inverse permutation {2,4,1,3,0}:
        // stores become unit-stride and the RHS elements are the permuted
        // ones. Store order within a block differs from the original, but
        // the five targets are disjoint, so the final a[] is identical.

        int lim = LEN_1D - LEN_1D % 5;
        for (int i = 0; i < lim; i += 5) {
            a[i]     = b[i + 2] + c[i + 2] * d[i + 2];
            a[i + 1] = b[i + 4] + c[i + 4] * d[i + 4];
            a[i + 2] = b[i + 1] + c[i + 1] * d[i + 1];
            a[i + 3] = b[i + 3] + c[i + 3] * d[i + 3];
            a[i + 4] = b[i]     + c[i]     * d[i];
        }
        for (int i = lim; i < LEN_1D; i++) {
            a[ip[i]] = b[i] + c[i] * d[i];
        }
        
        dummy(a, b, c, d, e, aa, bb, cc, 0.);
    }

    ROI_END(func_args);
    return calc_checksum(__func__);
}

// %4.11

//int s4112(int* __restrict__ ip, real_t s)
real_t s4112(struct args_t * func_args)
{

//    indirect addressing
//    sparse saxpy
//    gather is required

    struct{int * __restrict__ a;real_t b;} * x = func_args->arg_info;
    int * __restrict__ ip = x->a;
    real_t s = x->b;

    initialise_arrays(__func__);
    ROI_BEGIN(func_args);

    for (int nl = 0; nl < iterations; nl++) {
        /*
        for (int i = 0; i < LEN_1D; i++) {
            a[i] += b[ip[i]] * s;
        }
        */
        // Non-indexed equivalent (stock block-5 ip[] only, see s353 note;
        // 5-aligned prefix + scalar ip[] tail makes it LEN_1D-agnostic).
        // Same rewrite as s353, rolled form.

        int lim = LEN_1D - LEN_1D % 5;
        for (int i = 0; i < lim; i += 5) {
            a[i]     += b[i + 4] * s;
            a[i + 1] += b[i + 2] * s;
            a[i + 2] += b[i]     * s;
            a[i + 3] += b[i + 3] * s;
            a[i + 4] += b[i + 1] * s;
        }
        for (int i = lim; i < LEN_1D; i++) {
            a[i] += b[ip[i]] * s;
        }
        
        dummy(a, b, c, d, e, aa, bb, cc, 0.);
    }

    ROI_END(func_args);
    return calc_checksum(__func__);
}

// %4.11

//int s4113(int* __restrict__ ip)
real_t s4113(struct args_t * func_args)
{

//    indirect addressing
//    indirect addressing on rhs and lhs
//    gather and scatter is required

    int * __restrict__ ip = func_args->arg_info;

    initialise_arrays(__func__);
    ROI_BEGIN(func_args);

    for (int nl = 0; nl < iterations; nl++) {
        /*
        for (int i = 0; i < LEN_1D; i++) {
            a[ip[i]] = b[ip[i]] + c[i];
        }
        */
        // Non-indexed equivalent (stock block-5 ip[] only, see s353 note;
        // 5-aligned prefix + scalar ip[] tail makes it LEN_1D-agnostic).
        // Change of variable j = ip[i]: a[j] = b[j] + c[ipinv(j)], so both
        // the gather and the scatter disappear — a and b are unit-stride and
        // only c is read through the inverse permutation {2,4,1,3,0}.

        int lim = LEN_1D - LEN_1D % 5;
        for (int i = 0; i < lim; i += 5) {
            a[i]     = b[i]     + c[i + 2];
            a[i + 1] = b[i + 1] + c[i + 4];
            a[i + 2] = b[i + 2] + c[i + 1];
            a[i + 3] = b[i + 3] + c[i + 3];
            a[i + 4] = b[i + 4] + c[i];
        }
        for (int i = lim; i < LEN_1D; i++) {
            a[ip[i]] = b[ip[i]] + c[i];
        }
        
        dummy(a, b, c, d, e, aa, bb, cc, 0.);
    }

    ROI_END(func_args);
    return calc_checksum(__func__);
}

// %4.11

//int s4114(int* ip, int n1)
real_t s4114(struct args_t * func_args)
{

//    indirect addressing
//    mix indirect addressing with variable lower and upper bounds
//    gather is required

    struct{int * __restrict__ a;int b;} * x = func_args->arg_info;
    int * __restrict__ ip = x->a;
    int n1 = x->b;

    initialise_arrays(__func__);
    ROI_BEGIN(func_args);

    int k;
    for (int nl = 0; nl < iterations; nl++) {
        /*
        for (int i = n1-1; i < LEN_1D; i++) {
            k = ip[i];
            a[i] = b[i] + c[LEN_1D-k+1-2] * d[i];
            k += 5;
        }
        */
        // Non-indexed equivalent (stock block-5 ip[] only, see s353 note).
        // Size- and n1-independent, and ip[] is never read. Intrinsics
        // (precedent: s4117_vrgather) because c's 5-element window walks
        // BACKWARD one block per group — load-lanes (vlseg5) cannot express
        // that, and every autovectorized form is either a computed-index
        // vluxei gather or element-serial strided vlse. Here all four
        // streams are full-width unit-stride vle32/vse32; the reverse +
        // block permutation happens in-register via vrgather with the
        // loop-invariant selector sel[t] = VL-1 - (t-t%5) - p[t%5],
        // p = {4,2,0,3,1}. VL is capped to a multiple of 5 so blocks never
        // straddle a register; head/leftovers run scalar via IP_STOCK,
        // pinned with novector so no gather can reappear. Note: like
        // s4117_vrgather, the novec build of this kernel stays vector code.

        int beg = n1 - 1;
        int first = beg + (5 - beg % 5) % 5;
        if (first > LEN_1D) first = LEN_1D;
        int lim = first + (LEN_1D - first) / 5 * 5;
        #pragma GCC novector
        for (int i = beg; i < first; i++) {
            a[i] = b[i] + c[LEN_1D - IP_STOCK(i) + 1 - 2] * d[i];
        }
        int i = first;
        size_t vlmax = __riscv_vsetvlmax_e32m1();
        size_t VL = vlmax - vlmax % 5;
        if (VL >= 5) {
            uint32_t selbuf[vlmax];
            // novector: vectorized, this fill emits a vnsrl (64->32 narrow)
            // whose VPinVd + two half-writes on one pinned phys reg panic
            // gem5's O3 dependency graph ("Dependency graph ... not empty",
            // upstream latent bug). One-time <= vlmax scalar iterations.
            #pragma GCC novector
            for (size_t t = 0; t < VL; t++)
                selbuf[t] = VL - 1 - (t - t % 5) - (4 + 3 * (t % 5)) % 5;
            vuint32m1_t sel = __riscv_vle32_v_u32m1(selbuf, VL);
            for (; i + (int)VL <= lim; i += VL) {
                vfloat32m1_t vb = __riscv_vle32_v_f32m1(&b[i], VL);
                vfloat32m1_t vd = __riscv_vle32_v_f32m1(&d[i], VL);
                vfloat32m1_t vc =
                    __riscv_vle32_v_f32m1(&c[LEN_1D - i - (int)VL], VL);
                vfloat32m1_t vcx = __riscv_vrgather_vv_f32m1(vc, sel, VL);
                __riscv_vse32_v_f32m1(&a[i],
                    __riscv_vfmacc_vv_f32m1(vb, vcx, vd, VL), VL);
            }
        }
        #pragma GCC novector
        for (; i < LEN_1D; i++) {
            a[i] = b[i] + c[LEN_1D - IP_STOCK(i) + 1 - 2] * d[i];
        }
        
        dummy(a, b, c, d, e, aa, bb, cc, 0.);
    }

    ROI_END(func_args);
    return calc_checksum(__func__);
}

// %4.11

//int s4115(int* __restrict__ ip)
real_t s4115(struct args_t * func_args)
{

//    indirect addressing
//    sparse dot product
//    gather is required

    int * __restrict__ ip = func_args->arg_info;

    initialise_arrays(__func__);
    ROI_BEGIN(func_args);

    real_t sum;
    for (int nl = 0; nl < iterations; nl++) {
        sum = 0.;
        /*
        for (int i = 0; i < LEN_1D; i++) {
            sum += a[i] * b[ip[i]];
        }
        */
        // Non-indexed equivalent (stock block-5 ip[] only, see s353 note).
        // Intrinsics (precedent: s4117_vrgather): without -ffast-math the
        // autovectorizer must keep the sum in exact i-order, which zigzags
        // across the 5 fields, so the best it can do is vl=2 SLP fragments.
        // Here b's block is loaded unit-stride full-width, permuted
        // in-register (vrgather, selector sel[t] = (t-t%5) + p[t%5]), and
        // each strip is folded with the ordered vfredosum seeded by the
        // running sum — bitwise-identical to the original i-order result.
        // Leftovers (only when VL does not divide the trip count) run
        // scalar via IP_STOCK, pinned novector. Note: like s4117_vrgather,
        // the novec build of this kernel stays vector code.

        int lim = LEN_1D - LEN_1D % 5;
        int i = 0;
        size_t vlmax = __riscv_vsetvlmax_e32m1();
        size_t VL = vlmax - vlmax % 5;
        if (VL >= 5) {
            uint32_t selbuf[vlmax];
            // novector: see s4114 selbuf note (gem5 vnsrl panic).
            #pragma GCC novector
            for (size_t t = 0; t < VL; t++)
                selbuf[t] = (t - t % 5) + (4 + 3 * (t % 5)) % 5;
            vuint32m1_t sel = __riscv_vle32_v_u32m1(selbuf, VL);
            vfloat32m1_t vsum = __riscv_vfmv_s_f_f32m1(sum, 1);
            for (; i + (int)VL <= lim; i += VL) {
                vfloat32m1_t va = __riscv_vle32_v_f32m1(&a[i], VL);
                vfloat32m1_t vb = __riscv_vle32_v_f32m1(&b[i], VL);
                vfloat32m1_t vbx = __riscv_vrgather_vv_f32m1(vb, sel, VL);
                vfloat32m1_t vp = __riscv_vfmul_vv_f32m1(va, vbx, VL);
                vsum = __riscv_vfredosum_vs_f32m1_f32m1(vp, vsum, VL);
            }
            sum = __riscv_vfmv_f_s_f32m1_f32(vsum);
        }
        #pragma GCC novector
        for (; i < LEN_1D; i++) {
            sum += a[i] * b[IP_STOCK(i)];
        }
        
        dummy(a, b, c, d, e, aa, bb, cc, 0.);
    }

    ROI_END(func_args);
    return sum;
}

// %4.11

//int s4116(int* __restrict__ ip, int j, int inc)
real_t s4116(struct args_t * func_args)
{

//    indirect addressing
//    more complicated sparse sdot
//    gather is required

    struct{int * __restrict__ a;int b;int c;} * x = func_args->arg_info;
    int * __restrict__ ip = x->a;
    int j = x->b;
    int inc = x->c;

    initialise_arrays(__func__);
    ROI_BEGIN(func_args);

    real_t sum;
    int off;
    // vrgather selector for the block permutation (same as s4115), hoisted
    // out of the 100-rep timing loop.
    size_t vlmax = __riscv_vsetvlmax_e32m1();
    size_t VL = vlmax - vlmax % 5;
    uint32_t selbuf[vlmax];
    // novector: see s4114 selbuf note (gem5 vnsrl panic).
    #pragma GCC novector
    for (size_t t = 0; t < VL; t++)
        selbuf[t] = (t - t % 5) + (4 + 3 * (t % 5)) % 5;
    for (int nl = 0; nl < 100*iterations; nl++) {
        sum = 0.;
        /*
        for (int i = 0; i < LEN_2D-1; i++) {
            off = inc + i;
            sum += a[off] * aa[j-1][ip[i]];
        }
        */
        // Non-indexed equivalent (stock block-5 ip[] only, see s353 note).
        // Same intrinsics shape as s4115 applied to row aa[j-1]; the
        // ordered vfredosum keeps the sum bitwise in i-order. Two per-rep
        // costs matter at 100 reps: (1) sel is re-loaded from selbuf INSIDE
        // the loop — kept live across dummy() it gets spilled/reloaded with
        // csrr-vlenb addressing, and csrr is IsSerializeAfter in gem5, which
        // drains the pipeline twice per rep and stops reps overlapping;
        // (2) the last strip shrinks to the remaining multiple of 5, so
        // only the two straddle elements run scalar (via IP_STOCK, which
        // reproduces the original's read past LEN_2D-1 there). off drops
        // out. Note: the novec build of this kernel stays vector code.

        real_t * row = aa[j-1];
        int lim = (LEN_2D-1) - (LEN_2D-1) % 5;
        int i = 0;
        vuint32m1_t sel = __riscv_vle32_v_u32m1(selbuf, VL);
        vfloat32m1_t vsum = __riscv_vfmv_s_f_f32m1(sum, 1);
        while (VL >= 5 && lim - i >= 5) {
            size_t vl = (size_t)(lim - i) < VL ? (size_t)(lim - i) : VL;
            vfloat32m1_t va = __riscv_vle32_v_f32m1(&a[inc + i], vl);
            vfloat32m1_t vr = __riscv_vle32_v_f32m1(&row[i], vl);
            vfloat32m1_t vrx = __riscv_vrgather_vv_f32m1(vr, sel, vl);
            vfloat32m1_t vp = __riscv_vfmul_vv_f32m1(va, vrx, vl);
            vsum = __riscv_vfredosum_vs_f32m1_f32m1(vp, vsum, vl);
            i += vl;
        }
        sum = __riscv_vfmv_f_s_f32m1_f32(vsum);
        #pragma GCC novector
        for (; i < LEN_2D-1; i++) {
            sum += a[inc + i] * row[IP_STOCK(i)];
        }

        dummy(a, b, c, d, e, aa, bb, cc, 0.);
    }

    ROI_END(func_args);
    return sum;
}

// %4.11

real_t s4117(struct args_t * func_args)
{

//    indirect addressing
//    seq function

    initialise_arrays(__func__);
    ROI_BEGIN(func_args);

    for (int nl = 0; nl < iterations; nl++) {
        for (int i = 0; i < LEN_1D; i++) {
            a[i] = b[i] + c[i/2] * d[i];
        }
        dummy(a, b, c, d, e, aa, bb, cc, 0.);
    }

    ROI_END(func_args);
    return calc_checksum(__func__);
}

// %4.11

real_t s4117_modified(struct args_t * func_args)
{

//    indirect addressing
//    seq function

    initialise_arrays(__func__);
    ROI_BEGIN(func_args);
    
    for (int nl = 0; nl < iterations; nl++) {
        for (int i = 0; i < LEN_1D/2; i++) {
            for (int j = 0; j < 2; j++) {
                a[2*i+j] = b[2*i+j] + c[i] * d[2*i+j];
            }
        }
        dummy(a, b, c, d, e, aa, bb, cc, 0.);
    }
    
    ROI_END(func_args);
    return calc_checksum(__func__);
}

real_t s4117_vrgather(struct args_t * func_args)
{

//    indirect addressing
//    seq function
//    c[i/2] done as a unit-stride vle32 of c plus an in-register
//    vrgather with selector [0,0,1,1,2,2,...] (= vid >> 1), instead of
//    the vluxei gather (s4117) or segment loads (s4117_modified).
//    Intrinsics because the autovectorizer cannot produce this shape;
//    note -fno-tree-vectorize does NOT scalarize intrinsics, so the
//    novec build of this kernel is still vector code.

    initialise_arrays(__func__);
    ROI_BEGIN(func_args);

    for (int nl = 0; nl < iterations; nl++) {
        size_t i = 0;
        // i stays even: vl from vsetvl e32m1 is even for VLEN >= 64
        // and LEN_1D is even, so c[(i+j)/2] == c[i/2 + (j>>1)] exactly.
        while (i < LEN_1D) {
            size_t vl = __riscv_vsetvl_e32m1(LEN_1D - i);
            vuint32m1_t sel =
                __riscv_vsrl_vx_u32m1(__riscv_vid_v_u32m1(vl), 1, vl);
            vfloat32m1_t vc  = __riscv_vle32_v_f32m1(&c[i/2], vl);
            vfloat32m1_t vcx = __riscv_vrgather_vv_f32m1(vc, sel, vl);
            vfloat32m1_t vb  = __riscv_vle32_v_f32m1(&b[i], vl);
            vfloat32m1_t vd  = __riscv_vle32_v_f32m1(&d[i], vl);
            __riscv_vse32_v_f32m1(&a[i],
                __riscv_vfmacc_vv_f32m1(vb, vcx, vd, vl), vl);
            i += vl;
        }
        dummy(a, b, c, d, e, aa, bb, cc, 0.);
    }

    ROI_END(func_args);
    return calc_checksum(__func__);
}

// %4.12

real_t f(real_t a, real_t b){
    return a*b;
}

real_t s4121(struct args_t * func_args)
{

//    statement functions
//    elementwise multiplication

    initialise_arrays(__func__);
    ROI_BEGIN(func_args);

    for (int nl = 0; nl < iterations; nl++) {
        for (int i = 0; i < LEN_1D; i++) {
            a[i] += f(b[i],c[i]);
        }
        dummy(a, b, c, d, e, aa, bb, cc, 0.);
    }

    ROI_END(func_args);
    return calc_checksum(__func__);
}

// %5.1

real_t va(struct args_t * func_args)
{

//    control loops
//    vector assignment

    initialise_arrays(__func__);
    ROI_BEGIN(func_args);

    for (int nl = 0; nl < iterations*10; nl++) {
        for (int i = 0; i < LEN_1D; i++) {
            a[i] = b[i];
        }
        dummy(a, b, c, d, e, aa, bb, cc, 0.);
    }

    ROI_END(func_args);
    return calc_checksum(__func__);
}

// %5.1

//int vag( int* __restrict__ ip)
real_t vag(struct args_t * func_args)
{

//    control loops
//    vector assignment, gather
//    gather is required

    int * __restrict__ ip = func_args->arg_info;

    initialise_arrays(__func__);
    ROI_BEGIN(func_args);

    for (int nl = 0; nl < 2*iterations; nl++) {
        for (int i = 0; i < LEN_1D; i++) {
            a[i] = b[ip[i]];
        }
        dummy(a, b, c, d, e, aa, bb, cc, 0.);
    }

    ROI_END(func_args);
    return calc_checksum(__func__);
}

// %5.1

//int vas( int* __restrict__ ip)
real_t vas(struct args_t * func_args)
{

//    control loops
//    vector assignment, scatter
//    scatter is required

    int * __restrict__ ip = func_args->arg_info;

    initialise_arrays(__func__);
    ROI_BEGIN(func_args);

    for (int nl = 0; nl < 2*iterations; nl++) {
        for (int i = 0; i < LEN_1D; i++) {
            a[ip[i]] = b[i];
        }
        dummy(a, b, c, d, e, aa, bb, cc, 0.);
    }

    ROI_END(func_args);
    return calc_checksum(__func__);
}

// %5.1

real_t vif(struct args_t * func_args)
{

//    control loops
//    vector if

    initialise_arrays(__func__);
    ROI_BEGIN(func_args);

    for (int nl = 0; nl < iterations; nl++) {
        for (int i = 0; i < LEN_1D; i++) {
            if (b[i] > (real_t)0.) {
                a[i] = b[i];
            }
        }
        dummy(a, b, c, d, e, aa, bb, cc, 0.);
    }

    ROI_END(func_args);
    return calc_checksum(__func__);
}

// %5.1

real_t vpv(struct args_t * func_args)
{

//    control loops
//    vector plus vector

    initialise_arrays(__func__);
    ROI_BEGIN(func_args);

    for (int nl = 0; nl < iterations*10; nl++) {
        for (int i = 0; i < LEN_1D; i++) {
            a[i] += b[i];
        }
        dummy(a, b, c, d, e, aa, bb, cc, 0.);
    }

    ROI_END(func_args);
    return calc_checksum(__func__);
}

// %5.1

real_t vtv(struct args_t * func_args)
{

//    control loops
//    vector times vector

    initialise_arrays(__func__);
    ROI_BEGIN(func_args);

    for (int nl = 0; nl < iterations*10; nl++) {
        for (int i = 0; i < LEN_1D; i++) {
            a[i] *= b[i];
        }
        dummy(a, b, c, d, e, aa, bb, cc, 0.);
    }

    ROI_END(func_args);
    return calc_checksum(__func__);
}

// %5.1

real_t vpvtv(struct args_t * func_args)
{

//    control loops
//    vector plus vector times vector

    initialise_arrays(__func__);
    ROI_BEGIN(func_args);

    for (int nl = 0; nl < 4*iterations; nl++) {
        for (int i = 0; i < LEN_1D; i++) {
            a[i] += b[i] * c[i];
        }
        dummy(a, b, c, d, e, aa, bb, cc, 0.);
    }

    ROI_END(func_args);
    return calc_checksum(__func__);
}

// %5.1

//real_t vpvts( real_t s)
real_t vpvts(struct args_t * func_args)
{

//    control loops
//    vector plus vector times scalar

    real_t s = *(int*)func_args->arg_info;

    initialise_arrays(__func__);
    ROI_BEGIN(func_args);

    for (int nl = 0; nl < iterations; nl++) {
        for (int i = 0; i < LEN_1D; i++) {
            a[i] += b[i] * s;
        }
        dummy(a, b, c, d, e, aa, bb, cc, 0.);
    }

    ROI_END(func_args);
    return calc_checksum(__func__);
}

// %5.1

real_t vpvpv(struct args_t * func_args)
{

//    control loops
//    vector plus vector plus vector

    initialise_arrays(__func__);
    ROI_BEGIN(func_args);

    for (int nl = 0; nl < 4*iterations; nl++) {
        for (int i = 0; i < LEN_1D; i++) {
            a[i] += b[i] + c[i];
        }
        dummy(a, b, c, d, e, aa, bb, cc, 0.);
    }

    ROI_END(func_args);
    return calc_checksum(__func__);
}

// %5.1

real_t vtvtv(struct args_t * func_args)
{

//    control loops
//    vector times vector times vector

    initialise_arrays(__func__);
    ROI_BEGIN(func_args);

    for (int nl = 0; nl < 4*iterations; nl++) {
        for (int i = 0; i < LEN_1D; i++) {
            a[i] = a[i] * b[i] * c[i];
        }
        dummy(a, b, c, d, e, aa, bb, cc, 0.);
    }

    ROI_END(func_args);
    return calc_checksum(__func__);
}

// %5.1

real_t vsumr(struct args_t * func_args)
{

//    control loops
//    vector sum reduction

    initialise_arrays(__func__);
    ROI_BEGIN(func_args);

    real_t sum;
    for (int nl = 0; nl < iterations*10; nl++) {
        sum = 0.;
        for (int i = 0; i < LEN_1D; i++) {
            sum += a[i];
        }
        dummy(a, b, c, d, e, aa, bb, cc, sum);
    }

    ROI_END(func_args);
    return sum;
}

// %5.1

real_t vdotr(struct args_t * func_args)
{

//    control loops
//    vector dot product reduction

    initialise_arrays(__func__);
    ROI_BEGIN(func_args);

    real_t dot;
    for (int nl = 0; nl < iterations*10; nl++) {
        dot = 0.;
        for (int i = 0; i < LEN_1D; i++) {
            dot += a[i] * b[i];
        }
        dummy(a, b, c, d, e, aa, bb, cc, dot);
    }

    ROI_END(func_args);
    return dot;
}

// %5.1

real_t vbor(struct args_t * func_args)
{

//    control loops
//    basic operations rates, isolate arithmetic from memory traffic
//    all combinations of three, 59 flops for 6 loads and 1 store.

    initialise_arrays(__func__);
    ROI_BEGIN(func_args);

    real_t a1, b1, c1, d1, e1, f1;
    for (int nl = 0; nl < iterations*10; nl++) {
        for (int i = 0; i < LEN_2D; i++) {
            a1 = a[i];
            b1 = b[i];
            c1 = c[i];
            d1 = d[i];
            e1 = e[i];
            f1 = aa[0][i];
            a1 = a1 * b1 * c1 + a1 * b1 * d1 + a1 * b1 * e1 + a1 * b1 * f1 +
                a1 * c1 * d1 + a1 * c1 * e1 + a1 * c1 * f1 + a1 * d1 * e1
                + a1 * d1 * f1 + a1 * e1 * f1;
            b1 = b1 * c1 * d1 + b1 * c1 * e1 + b1 * c1 * f1 + b1 * d1 * e1 +
                b1 * d1 * f1 + b1 * e1 * f1;
            c1 = c1 * d1 * e1 + c1 * d1 * f1 + c1 * e1 * f1;
            d1 = d1 * e1 * f1;
            x[i] = a1 * b1 * c1 * d1;
        }
        dummy(a, b, c, d, e, aa, bb, cc, 0.);
    }

    ROI_END(func_args);
    return calc_checksum(__func__);
}

typedef real_t(*test_function_t)(struct args_t *);

/* Subset-selection filter: if non-zero, only named kernels are run. */
static int g_num_filters = 0;
static char **g_filters = NULL;

static int should_run(const char *name)
{
    if (g_num_filters == 0) return 1;
    for (int i = 0; i < g_num_filters; i++) {
        if (strcmp(name, g_filters[i]) == 0) return 1;
    }
    return 0;
}

void time_function(const char *name, test_function_t vector_func, void * arg_info)
{
    struct args_t func_args = {.arg_info=arg_info};

    double result = vector_func(&func_args);

    double tic=func_args.t1.tv_sec+(func_args.t1.tv_usec/1000000.0);
    double toc=func_args.t2.tv_sec+(func_args.t2.tv_usec/1000000.0);

    double taken = toc-tic;
    uint64_t cycles = func_args.c2 - func_args.c1;

    printf("%-12s\t%10.3f\t%12" PRIu64 "\t%f\n", name, taken, cycles, result);
}

/* RUN_KERNEL: stringifies the function name, checks filter, then times it. */
#define RUN_KERNEL(func, ...) \
    do { if (should_run(#func)) time_function(#func, &func, __VA_ARGS__); } while (0)

int main(int argc, char ** argv){
    /* Newlib buffers stdout when not connected to a terminal (e.g. gem5 SE
     * mode).  Disable buffering so every printf is immediately visible. */
    setvbuf(stdout, NULL, _IONBF, 0);

#ifdef TSVC_KERNELS
    /* Kernel whitelist baked in at compile time.
     * Build with: make KERNELS="s000 s111 va"
     * which passes -DTSVC_KERNELS='"s000 s111 va"' to the compiler. */
    static char kernel_list[] = TSVC_KERNELS;
    static char *kernel_tokens[256];
    {
        int n = 0;
        char *tok = strtok(kernel_list, " ,");
        while (tok && n < (int)(sizeof(kernel_tokens)/sizeof(kernel_tokens[0]))) {
            kernel_tokens[n++] = tok;
            tok = strtok(NULL, " ,");
        }
        g_num_filters = n;
        g_filters = kernel_tokens;
    }
#else
    /* Runtime fallback: argv[1], argv[2], ... are kernel names to run.
     * Used by run-tsvc.sh, which passes the kernel name via gem5's --parms. */
    if (argc > 1) {
        g_num_filters = argc - 1;
        g_filters = argv + 1;
    }
#endif

    int n1 = 1;
    int n3 = 1;
    int* ip;
    real_t s1,s2;
    init(&ip, &s1, &s2);
    printf("%-12s\t%10s\t%12s\t%s\n", "Loop", "Time(sec)", "Cycles", "Checksum");

    RUN_KERNEL(s000, NULL);
    RUN_KERNEL(s111, NULL);
    RUN_KERNEL(s1111, NULL);
    RUN_KERNEL(s112, NULL);
    RUN_KERNEL(s1112, NULL);
    RUN_KERNEL(s113, NULL);
    RUN_KERNEL(s1113, NULL);
    RUN_KERNEL(s114, NULL);
    RUN_KERNEL(s115, NULL);
    RUN_KERNEL(s1115, NULL);
    RUN_KERNEL(s116, NULL);
    RUN_KERNEL(s118, NULL);
    RUN_KERNEL(s119, NULL);
    RUN_KERNEL(s1119, NULL);
    RUN_KERNEL(s121, NULL);
    RUN_KERNEL(s122, &(struct{int a;int b;}){n1, n3});
    RUN_KERNEL(s123, NULL);
    RUN_KERNEL(s124, NULL);
    RUN_KERNEL(s125, NULL);
    RUN_KERNEL(s126, NULL);
    RUN_KERNEL(s127, NULL);
    RUN_KERNEL(s128, NULL);
    RUN_KERNEL(s131, NULL);
    RUN_KERNEL(s132, NULL);
    RUN_KERNEL(s141, NULL);
    RUN_KERNEL(s151, NULL);
    RUN_KERNEL(s152, NULL);
    RUN_KERNEL(s161, NULL);
    RUN_KERNEL(s1161, NULL);
    RUN_KERNEL(s162, &n1);
    RUN_KERNEL(s171, &n1);
    RUN_KERNEL(s172, &(struct{int a;int b;}){n1, n3});
    RUN_KERNEL(s173, NULL);
    RUN_KERNEL(s174, &(struct{int a;}){LEN_1D/2});
    RUN_KERNEL(s175, &n1);
    RUN_KERNEL(s176, NULL);
    RUN_KERNEL(s211, NULL);
    RUN_KERNEL(s212, NULL);
    RUN_KERNEL(s1213, NULL);
    RUN_KERNEL(s221, NULL);
    RUN_KERNEL(s1221, NULL);
    RUN_KERNEL(s222, NULL);
    RUN_KERNEL(s231, NULL);
    RUN_KERNEL(s232, NULL);
    RUN_KERNEL(s1232, NULL);
    RUN_KERNEL(s233, NULL);
    RUN_KERNEL(s2233, NULL);
    RUN_KERNEL(s235, NULL);
    RUN_KERNEL(s241, NULL);
    RUN_KERNEL(s242, &(struct{real_t a;real_t b;}){s1, s2});
    RUN_KERNEL(s243, NULL);
    RUN_KERNEL(s244, NULL);
    RUN_KERNEL(s1244, NULL);
    RUN_KERNEL(s2244, NULL);
    RUN_KERNEL(s251, NULL);
    RUN_KERNEL(s1251, NULL);
    RUN_KERNEL(s2251, NULL);
    RUN_KERNEL(s3251, NULL);
    RUN_KERNEL(s252, NULL);
    RUN_KERNEL(s253, NULL);
    RUN_KERNEL(s254, NULL);
    RUN_KERNEL(s255, NULL);
    RUN_KERNEL(s256, NULL);
    RUN_KERNEL(s257, NULL);
    RUN_KERNEL(s258, NULL);
    RUN_KERNEL(s261, NULL);
    RUN_KERNEL(s271, NULL);
    RUN_KERNEL(s272, &s1);
    RUN_KERNEL(s273, NULL);
    RUN_KERNEL(s274, NULL);
    RUN_KERNEL(s275, NULL);
    RUN_KERNEL(s2275, NULL);
    RUN_KERNEL(s276, NULL);
    RUN_KERNEL(s277, NULL);
    RUN_KERNEL(s278, NULL);
    RUN_KERNEL(s279, NULL);
    RUN_KERNEL(s1279, NULL);
    RUN_KERNEL(s2710, &s1);
    RUN_KERNEL(s2711, NULL);
    RUN_KERNEL(s2712, NULL);
    RUN_KERNEL(s281, NULL);
    RUN_KERNEL(s1281, NULL);
    RUN_KERNEL(s291, NULL);
    RUN_KERNEL(s292, NULL);
    RUN_KERNEL(s293, NULL);
    RUN_KERNEL(s2101, NULL);
    RUN_KERNEL(s2102, NULL);
    RUN_KERNEL(s2111, NULL);
    RUN_KERNEL(s311, NULL);
    RUN_KERNEL(s31111, NULL);
    RUN_KERNEL(s312, NULL);
    RUN_KERNEL(s313, NULL);
    RUN_KERNEL(s314, NULL);
    RUN_KERNEL(s315, NULL);
    RUN_KERNEL(s316, NULL);
    RUN_KERNEL(s317, NULL);
    RUN_KERNEL(s318, &n1);
    RUN_KERNEL(s319, NULL);
    RUN_KERNEL(s3110, NULL);
    RUN_KERNEL(s13110, NULL);
    RUN_KERNEL(s3111, NULL);
    RUN_KERNEL(s3112, NULL);
    RUN_KERNEL(s3113, NULL);
    RUN_KERNEL(s321, NULL);
    RUN_KERNEL(s322, NULL);
    RUN_KERNEL(s323, NULL);
    RUN_KERNEL(s331, NULL);
    RUN_KERNEL(s332, &s1);
    RUN_KERNEL(s341, NULL);
    RUN_KERNEL(s342, NULL);
    RUN_KERNEL(s343, NULL);
    RUN_KERNEL(s351, NULL);
    RUN_KERNEL(s1351, NULL);
    RUN_KERNEL(s352, NULL);
    RUN_KERNEL(s353, ip);
    RUN_KERNEL(s421, NULL);
    RUN_KERNEL(s1421, NULL);
    RUN_KERNEL(s422, NULL);
    RUN_KERNEL(s423, NULL);
    RUN_KERNEL(s424, NULL);
    RUN_KERNEL(s431, NULL);
    RUN_KERNEL(s441, NULL);
    RUN_KERNEL(s442, NULL);
    RUN_KERNEL(s443, NULL);
    RUN_KERNEL(s451, NULL);
    RUN_KERNEL(s452, NULL);
    RUN_KERNEL(s453, NULL);
    RUN_KERNEL(s471, NULL);
    RUN_KERNEL(s481, NULL);
    RUN_KERNEL(s482, NULL);
    RUN_KERNEL(s491, ip);
    RUN_KERNEL(s4112, &(struct{int*a;real_t b;}){ip, s1});
    RUN_KERNEL(s4113, ip);
    RUN_KERNEL(s4114, &(struct{int*a;int b;}){ip, n1});
    RUN_KERNEL(s4115, ip);
    RUN_KERNEL(s4116, &(struct{int * a; int b; int c;}){ip, LEN_2D/2, n1});
    RUN_KERNEL(s4117, NULL);
    RUN_KERNEL(s4117_modified, NULL);
    RUN_KERNEL(s4117_vrgather, NULL);
    RUN_KERNEL(s4121, NULL);
    RUN_KERNEL(va, NULL);
    RUN_KERNEL(vag, ip);
    RUN_KERNEL(vas, ip);
    RUN_KERNEL(vif, NULL);
    RUN_KERNEL(vpv, NULL);
    RUN_KERNEL(vtv, NULL);
    RUN_KERNEL(vpvtv, NULL);
    RUN_KERNEL(vpvts, &s1);
    RUN_KERNEL(vpvpv, NULL);
    RUN_KERNEL(vtvtv, NULL);
    RUN_KERNEL(vsumr, NULL);
    RUN_KERNEL(vdotr, NULL);
    RUN_KERNEL(vbor, NULL);

    return EXIT_SUCCESS;
}
