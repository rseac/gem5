#ifndef STREAMCLUSTER_H
#define STREAMCLUSTER_H

#include <stdint.h>
#include <stddef.h>

#define NUM_POINTS 16
#define DIM_POINTS 8
#define NUM_CENTERS 2

void streamcluster_init(int num_points, int dim, float *points, float *centers);
void streamcluster_vector(int num_points, int dim, int k_centers, float *points, float *centers, float *min_dists, int *assignments);
int streamcluster_verify(int num_points, float *min_dists);

#endif
