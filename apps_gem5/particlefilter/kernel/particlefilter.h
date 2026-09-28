#ifndef PARTICLEFILTER_H
#define PARTICLEFILTER_H

#include <stdint.h>
#include <stddef.h>

#define N_PARTICLES 128
#define N_STEPS 2

void particlefilter_init(int num_particles, float *weights, float *arrayX, float *arrayY);

void particlefilter_vector(int num_particles, int steps,
                           float *weights, float *arrayX, float *arrayY,
                           float *likelihood, float *xj, float *yj);

int particlefilter_verify(int num_particles, float *weights);

#endif
