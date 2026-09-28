#pragma GCC optimize ("no-tree-vectorize")
#include "streamcluster.h"
#include <math.h>

void streamcluster_init(int num_points, int dim, float *points, float *centers) {
    for (int i = 0; i < num_points * dim; i++) {
        points[i] = (float)((i * 17 + 5) % 100) / 10.0f;
    }
    for (int c = 0; c < NUM_CENTERS * dim; c++) {
        centers[c] = (float)((c * 23 + 11) % 100) / 10.0f;
    }
}

void streamcluster_vector(int num_points, int dim, int k_centers, float *points, float *centers, float *min_dists, int *assignments) {
    for (int p = 0; p < num_points; p++) {
        float *pt_ptr = &points[p * dim];
        float best_dist = 1e9f;
        int best_center = 0;

        for (int c = 0; c < k_centers; c++) {
            float *c_ptr = &centers[c * dim];
            float current_dist = 0.0f;

            int d = 0;
            while (d < dim) {
                size_t vl;
                asm volatile("vsetvli %[vl], %[cnt], e32, m1, ta, ma"
                             : [vl] "=r"(vl)
                             : [cnt] "r"(dim - d));

                asm volatile("vle32.v v2, (%0)" :: "r"(&pt_ptr[d]) : "v2", "memory");
                asm volatile("vle32.v v3, (%0)" :: "r"(&c_ptr[d]) : "v3", "memory");

                asm volatile("vfsub.vv v2, v2, v3" : : : "v2", "v3");
                asm volatile("vfmul.vv v2, v2, v2" : : : "v2");

                float scalar_zero = 0.0f;
                asm volatile("vfmv.s.f v4, %0" :: "f"(scalar_zero) : "v4");
                asm volatile("vfredusum.vs v1, v2, v4" : : : "v1", "v2", "v4");

                float red_result;
                asm volatile("vfmv.f.s %0, v1" : "=f"(red_result) : : "v1");
                current_dist += red_result;

                d += vl;
            }

            if (current_dist < best_dist) {
                best_dist = current_dist;
                best_center = c;
            }
        }

        min_dists[p] = best_dist;
        assignments[p] = best_center;
    }
}

int streamcluster_verify(int num_points, float *min_dists) {
    for (int i = 0; i < num_points; i++) {
        if (min_dists[i] < 0.0f || min_dists[i] > 1e8f) {
            return i + 1;
        }
    }
    return 0;
}
