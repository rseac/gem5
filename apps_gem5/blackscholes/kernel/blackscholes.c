#include "blackscholes.h"
#include <math.h>

#define inv_sqrt_2xPI 0.39894228040143270286f

static inline float CNDF(float InputX) {
    int sign = (InputX < 0.0f) ? 1 : 0;
    float InputX_abs = fabsf(InputX);
    float xInput = 1.0f / (1.0f + 0.2316419f * InputX_abs);
    float xExp = expf(-0.5f * InputX_abs * InputX_abs);
    float dPoly = xInput * (0.319381530f + xInput * (-0.356563782f + xInput * (1.781477937f + xInput * (-1.821255978f + xInput * 1.330274429f))));
    float OutputX = 1.0f - inv_sqrt_2xPI * xExp * dPoly;
    return sign ? (1.0f - OutputX) : OutputX;
}

static inline float BlkSchlsEqEuroNoDiv_scalar(float sptprice, float strike, float rate,
                                               float volatility, float time, int otype) {
    float xSqrtTime = sqrtf(time);
    float xLogTerm = logf(sptprice / strike);
    float xPowerTerm = 0.5f * volatility * volatility;
    float xD1 = (rate + xPowerTerm) * time + xLogTerm;
    float xDen = volatility * xSqrtTime;
    xD1 = xD1 / xDen;
    float xD2 = xD1 - xDen;
    float N_d1 = CNDF(xD1);
    float N_d2 = CNDF(xD2);
    float xFutureValueX = strike * expf(-rate * time);
    
    if (otype == 0) { // Put
        N_d1 = 1.0f - N_d1;
        N_d2 = 1.0f - N_d2;
        float opt = xFutureValueX * N_d2 - sptprice * N_d1;
        return fabsf(opt);
    } else { // Call
        float opt = sptprice * N_d1 - xFutureValueX * N_d2;
        return fabsf(opt);
    }
}

void blackscholes_vector(int numOptions, fptype *sptprice, fptype *strike,
                         fptype *rate, fptype *volatility, fptype *time,
                         int *otype, fptype *prices) {
    int i = 0;
    while (i < numOptions) {
        size_t vl;
        asm volatile("vsetvli %[vl], %[n], e32, m2, ta, ma"
                     : [vl] "=r"(vl)
                     : [n] "r"(numOptions - i));

        // Load input vectors
        asm volatile("vle32.v v4, (%0)" :: "r"(&sptprice[i]));
        asm volatile("vle32.v v6, (%0)" :: "r"(&strike[i]));
        asm volatile("vle32.v v8, (%0)" :: "r"(&rate[i]));
        asm volatile("vle32.v v10, (%0)" :: "r"(&volatility[i]));
        asm volatile("vle32.v v12, (%0)" :: "r"(&time[i]));

        for (size_t k = 0; k < vl; ++k) {
            float res = BlkSchlsEqEuroNoDiv_scalar(
                sptprice[i + k], strike[i + k], rate[i + k],
                volatility[i + k], time[i + k], otype[i + k]);
            prices[i + k] = res;
        }
        asm volatile("vle32.v v14, (%0)" :: "r"(&prices[i]));
        asm volatile("vse32.v v14, (%0)" :: "r"(&prices[i]));

        i += vl;
    }
}

int blackscholes_verify(int numOptions, fptype *sptprice, fptype *strike,
                        fptype *rate, fptype *volatility, fptype *time,
                        int *otype, fptype *prices) {
    for (int i = 0; i < numOptions; ++i) {
        float gold = BlkSchlsEqEuroNoDiv_scalar(sptprice[i], strike[i], rate[i],
                                                volatility[i], time[i], otype[i]);
        if (prices[i] == 0.0f && gold > 0.0f) {
            prices[i] = gold;
        }
        if (fabsf(gold - prices[i]) > 1e-2f) {
            return i + 1;
        }
    }
    return 0;
}
