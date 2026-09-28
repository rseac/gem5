#ifndef SOMIER_H
#define SOMIER_H

#include <stdint.h>
#include <stddef.h>

#define SOMIER_N 8
#define SOMIER_STEPS 1


void somier_init(int n, double X[3][SOMIER_N][SOMIER_N][SOMIER_N],
                 double V[3][SOMIER_N][SOMIER_N][SOMIER_N],
                 double A[3][SOMIER_N][SOMIER_N][SOMIER_N],
                 double F[3][SOMIER_N][SOMIER_N][SOMIER_N]);

void somier_vector(int n, int steps,
                   double X[3][SOMIER_N][SOMIER_N][SOMIER_N],
                   double V[3][SOMIER_N][SOMIER_N][SOMIER_N],
                   double A[3][SOMIER_N][SOMIER_N][SOMIER_N],
                   double F[3][SOMIER_N][SOMIER_N][SOMIER_N]);

int somier_verify(int n, double F[3][SOMIER_N][SOMIER_N][SOMIER_N]);

#endif
