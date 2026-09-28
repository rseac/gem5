#if defined(__GNUC__) || defined(__clang__)
#pragma GCC optimize ("no-tree-vectorize")
#endif

#include "somier.h"
#include <math.h>

static double dt = 0.001;
static double spring_K = 10.0;
static double mass = 1.0;

void somier_init(int n, double X[3][SOMIER_N][SOMIER_N][SOMIER_N],
                 double V[3][SOMIER_N][SOMIER_N][SOMIER_N],
                 double A[3][SOMIER_N][SOMIER_N][SOMIER_N],
                 double F[3][SOMIER_N][SOMIER_N][SOMIER_N]) {
    for (int d = 0; d < 3; d++) {
        for (int i = 0; i < n; i++) {
            for (int j = 0; j < n; j++) {
                for (int k = 0; k < n; k++) {
                    X[d][i][j][k] = (d == 0 ? i : (d == 1 ? j : k)) * 1.0;
                    V[d][i][j][k] = 0.0;
                    A[d][i][j][k] = 0.0;
                    F[d][i][j][k] = 0.0;
                }
            }
        }
    }
    // Perturb central node to generate spring lattice dynamics
    X[0][n / 2][n / 2][n / 2] += 0.2;
}

static inline void force_contr_vec_k(int n, double X[3][SOMIER_N][SOMIER_N][SOMIER_N],
                                     double F[3][SOMIER_N][SOMIER_N][SOMIER_N],
                                     int i, int j, int neig_i, int neig_j) {
    int k = 0;
    while (k < n) {
        size_t vl;
        asm volatile("vsetvli %[vl], %[cnt], e64, m1, ta, ma"
                     : [vl] "=r"(vl)
                     : [cnt] "r"(n - k));


        // Load X coordinates
        asm volatile("vle64.v v0, (%0)" :: "r"(&X[0][neig_i][neig_j][k]));
        asm volatile("vle64.v v1, (%0)" :: "r"(&X[0][i][j][k]));
        asm volatile("vfsub.vv v0, v0, v1"); // dx

        asm volatile("vle64.v v2, (%0)" :: "r"(&X[1][neig_i][neig_j][k]));
        asm volatile("vle64.v v3, (%0)" :: "r"(&X[1][i][j][k]));
        asm volatile("vfsub.vv v2, v2, v3"); // dy

        asm volatile("vle64.v v4, (%0)" :: "r"(&X[2][neig_i][neig_j][k]));
        asm volatile("vle64.v v5, (%0)" :: "r"(&X[2][i][j][k]));
        asm volatile("vfsub.vv v4, v4, v5"); // dz

        // dl = sqrt(dx^2 + dy^2 + dz^2)
        asm volatile("vfmul.vv v6, v0, v0");
        asm volatile("vfmacc.vv v6, v2, v2");
        asm volatile("vfmacc.vv v6, v4, v4");
        asm volatile("vfsqrt.v v6, v6"); // dl

        // spring_F = 0.25 * spring_K * (dl - 1.0)
        double c1 = 1.0;
        double c_k = 0.25 * spring_K;
        asm volatile("vfsub.vf v7, v6, %0" :: "f"(c1));
        asm volatile("vfmul.vf v7, v7, %0" :: "f"(c_k)); // spring_F

        // dF = spring_F / dl
        asm volatile("vfdiv.vv v7, v7, v6");

        // Load & accumulate F
        asm volatile("vle64.v v8, (%0)" :: "r"(&F[0][i][j][k]));
        asm volatile("vfmacc.vv v8, v7, v0");
        asm volatile("vse64.v v8, (%0)" :: "r"(&F[0][i][j][k]));

        asm volatile("vle64.v v9, (%0)" :: "r"(&F[1][i][j][k]));
        asm volatile("vfmacc.vv v9, v7, v2");
        asm volatile("vse64.v v9, (%0)" :: "r"(&F[1][i][j][k]));

        asm volatile("vle64.v v10, (%0)" :: "r"(&F[2][i][j][k]));
        asm volatile("vfmacc.vv v10, v7, v4");
        asm volatile("vse64.v v10, (%0)" :: "r"(&F[2][i][j][k]));

        k += vl;
    }
}

void somier_vector(int n, int steps,
                   double X[3][SOMIER_N][SOMIER_N][SOMIER_N],
                   double V[3][SOMIER_N][SOMIER_N][SOMIER_N],
                   double A[3][SOMIER_N][SOMIER_N][SOMIER_N],
                   double F[3][SOMIER_N][SOMIER_N][SOMIER_N]) {
    for (int s = 0; s < steps; s++) {
        for (int i = 1; i < n - 1; i++) {
            for (int j = 1; j < n - 1; j++) {
                force_contr_vec_k(n, X, F, i, j, i, j + 1);
                force_contr_vec_k(n, X, F, i, j, i - 1, j);
                force_contr_vec_k(n, X, F, i, j, i + 1, j);
                force_contr_vec_k(n, X, F, i, j, i, j - 1);
            }
        }
    }
}

int somier_verify(int n, double F[3][SOMIER_N][SOMIER_N][SOMIER_N]) {
    // Non-zero forces must exist at internal nodes
    double sum = 0.0;
    for (int i = 1; i < n - 1; i++) {
        for (int j = 1; j < n - 1; j++) {
            for (int k = 1; k < n - 1; k++) {
                sum += fabs(F[0][i][j][k]) + fabs(F[1][i][j][k]) + fabs(F[2][i][j][k]);
            }
        }
    }
    // Expected to be non-zero after spring interactions
    return (sum > 0.0) ? 0 : 1;
}
