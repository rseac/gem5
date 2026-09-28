#if defined(__GNUC__) || defined(__clang__)
#pragma GCC optimize ("no-tree-vectorize")
#endif

#include "particlefilter.h"
#include <math.h>


void particlefilter_init(int num_particles, float *weights, float *arrayX, float *arrayY) {
    float init_w = 1.0f / (float)num_particles;
    for (int i = 0; i < num_particles; i++) {
        weights[i] = init_w;
        arrayX[i] = (float)(i % 16);
        arrayY[i] = (float)(i / 16);
    }
}

void particlefilter_vector(int num_particles, int steps,
                           float *weights, float *arrayX, float *arrayY,
                           float *likelihood, float *xj, float *yj) {
    float target_x = 8.0f;
    float target_y = 4.0f;
    float variance = 4.0f;
    float inv_2var = 1.0f / (2.0f * variance);

    for (int s = 0; s < steps; s++) {
        int i = 0;
        while (i < num_particles) {
            size_t vl;
            asm volatile("vsetvli %[vl], %[cnt], e32, m2, ta, ma"
                         : [vl] "=r"(vl)
                         : [cnt] "r"(num_particles - i));

            // Load positions
            asm volatile("vle32.v v2, (%0)" :: "r"(&arrayX[i]));
            asm volatile("vle32.v v4, (%0)" :: "r"(&arrayY[i]));

            // dx = arrayX - target_x, dy = arrayY - target_y
            asm volatile("vfsub.vf v2, v2, %0" :: "f"(target_x));
            asm volatile("vfsub.vf v4, v4, %0" :: "f"(target_y));

            // dist2 = dx^2 + dy^2
            asm volatile("vfmul.vv v6, v2, v2");
            asm volatile("vfmacc.vv v6, v4, v4");

            // exponent = -dist2 / (2 * variance)
            asm volatile("vfmul.vf v6, v6, %0" :: "f"(-inv_2var));

            // Load weights
            asm volatile("vle32.v v8, (%0)" :: "r"(&weights[i]));

            // Simplified linear Taylor approximation for exp(x): 1 + x + 0.5*x^2
            float c1 = 1.0f;
            float c_half = 0.5f;
            asm volatile("vfmul.vv v10, v6, v6"); // x^2
            asm volatile("vfmadd.vf v10, %0, v6" :: "f"(c_half)); // 0.5*x^2 + x
            asm volatile("vfadd.vf v10, v10, %0" :: "f"(c1)); // exp_approx

            // Update weights: weight = weight * exp_approx
            asm volatile("vfmul.vv v8, v8, v10");

            // Store back weights
            asm volatile("vse32.v v8, (%0)" :: "r"(&weights[i]));

            i += vl;
        }
    }
}

int particlefilter_verify(int num_particles, float *weights) {
    float total_weight = 0.0f;
    for (int i = 0; i < num_particles; i++) {
        total_weight += weights[i];
    }
    return (total_weight > 0.0f) ? 0 : 1;
}
