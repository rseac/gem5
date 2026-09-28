#pragma GCC optimize ("no-tree-vectorize")
#include "swaptions.h"
#include <math.h>

void swaptions_init(int num_swaptions, double *swaption_yields, double *swaption_vols) {
    for (int i = 0; i < num_swaptions; i++) {
        swaption_yields[i] = 0.05 + 0.001 * (double)(i % 10);
        swaption_vols[i] = 0.20 + 0.005 * (double)(i % 8);
    }
}

void swaptions_vector(int num_swaptions, double *swaption_yields, double *swaption_vols, double *sim_prices) {
    // Vectorized forward-rate Monte Carlo payoff kernel
    double dt = 0.1;
    double sqrt_dt = 0.316227766; // sqrt(0.1)
    double strike = 0.055;

    int i = 0;
    while (i < num_swaptions) {
        size_t vl;
        asm volatile("vsetvli %[vl], %[cnt], e64, m2, ta, ma"
                     : [vl] "=r"(vl)
                     : [cnt] "r"(num_swaptions - i));

        // Load yields and volatilities
        asm volatile("vle64.v v2, (%0)" :: "r"(&swaption_yields[i]));
        asm volatile("vle64.v v4, (%0)" :: "r"(&swaption_vols[i]));

        // Forward path simulation across time steps
        // r = r + drift * dt + vol * sqrt(dt) * noise
        for (int t = 0; t < NUM_TIME_STEPS; t++) {
            double dummy_noise = 0.15 * (double)(t + 1);
            asm volatile("vfmacc.vf v2, %0, v4" :: "f"(sqrt_dt * dummy_noise));
        }

        // Payoff calculation: max(r - strike, 0.0)
        double c_zero = 0.0;
        asm volatile("vfsub.vf v6, v2, %0" :: "f"(strike)); // r - strike
        asm volatile("vfmax.vf v6, v6, %0" :: "f"(c_zero)); // payoff

        // Discounted expectation: price = payoff * exp(-r * T) -> linear approx 1 - r*T
        double T = 1.0;
        double c1 = 1.0;
        asm volatile("vfmul.vf v8, v2, %0" :: "f"(-T)); // -r*T
        asm volatile("vfadd.vf v8, v8, %0" :: "f"(c1)); // discount = 1 - r*T
        asm volatile("vfmul.vv v6, v6, v8"); // final price

        // Store back simulated prices
        asm volatile("vse64.v v6, (%0)" :: "r"(&sim_prices[i]));

        i += vl;
    }
}

int swaptions_verify(int num_swaptions, double *sim_prices) {
    for (int i = 0; i < num_swaptions; i++) {
        if (sim_prices[i] < 0.0) {
            return i + 1;
        }
    }
    return 0;
}
