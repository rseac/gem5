/*
 * Copyright (C) 2008 Princeton University
 * All rights reserved.
 * Authors: Jia Deng, Gilberto Contreras
 *
 * streamcluster - Online clustering algorithm
 *
 */

/*************************************************************************
* RISC-V Vectorized Version
* Author: Cristóbal Ramírez Lazo
* email: cristobal.ramirez@bsc.es
* Barcelona Supercomputing Center (2020)
*************************************************************************/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <math.h>
#include <sys/resource.h>
#include <limits.h>

#ifdef USE_RISCV_VECTOR
#include "common/vector_defines.h"
#endif

#include "common/riscv_util.h"

#include <time.h>
#include <sys/time.h>

#ifdef ENABLE_THREADS
#include <pthread.h>
#include "parsec_barrier.hpp"
#endif
#include "parsec_barrier.hpp"
#ifdef TBB_VERSION
#define TBB_STEALER (tbb::task_scheduler_init::occ_stealer)
#define NUM_DIVISIONS (nproc)
#include "tbb/task_scheduler_init.h"
#include "tbb/blocked_range.h"
#include "tbb/parallel_for.h"
#include "tbb/parallel_reduce.h"
#include "tbb/cache_aligned_allocator.h"
using namespace tbb;
#endif

#ifdef ENABLE_PARSEC_HOOKS
#include <hooks.h>
#endif

#include "printf.h"
#include "runtime.h"

using namespace std;

#define MAXNAMESIZE 1024 // max filename length
#define SEED 1
/* increase this to reduce probability of random error */
/* increasing it also ups running time of "speedy" part of the code */
/* SP = 1 seems to be fine */
#define SP 1 // number of repetitions of speedy must be >=1

/* higher ITER --> more likely to get correct # of centers */
/* higher ITER also scales the running time almost linearly */
#define ITER 3 // iterate ITER* k log k times; ITER >= 1

#define CACHE_LINE 64 // cache line in byte

/* this structure represents a point */
/* these will be passed around to avoid copying coordinates */
typedef struct {
  float weight;
  float *coord;
  long assign;  /* number of point where this one is assigned */
  float cost;  /* cost of that assignment, weight*distance */
} Point;

/* this is the array of points */
typedef struct {
  long num; /* number of points; may not be N if this is a sample */
  int dim;  /* dimensionality */
  Point *p; /* the array itself */
} Points;

static bool *switch_membership; //whether to switch membership in pgain
static bool* is_center; //whether a point is a center
static int* center_table; //index table of centers

const int nproc = 1; //# of threads

// Parameters and data from the statically compiled data.S

extern int KMIN;
extern int KMAX;
extern int DIM;
extern int N;
extern int CHUNKSIZE;
extern int CLUSTERSIZE;
extern float block[] __attribute__((aligned(4 * NR_LANES * NR_CLUSTERS)));

#ifdef TBB_VERSION
tbb::cache_aligned_allocator<float> memoryFloat;
tbb::cache_aligned_allocator<Point> memoryPoint;
tbb::cache_aligned_allocator<long> memoryLong;
tbb::cache_aligned_allocator<int> memoryInt;
tbb::cache_aligned_allocator<bool> memoryBool;
#endif

static int cnt_dist=0; // for counting number of distance calculations
float dist(Point p1, Point p2, int dim);


#ifdef TBB_VERSION

#endif //TBB_VERSION
/********************************************/



int isIdentical(float *i, float *j, int D)
// tells whether two points of D dimensions are identical
{
  int a = 0;
  int equal = 1;

  while (equal && a < D) {
    if (i[a] != j[a]) equal = 0;
    else a++;
  }
  if (equal) return 1;
  else return 0;

}

/* comparator for floating point numbers */
static int floatcomp(const void *i, const void *j)
{
  float a, b;
  a = *(float *)(i);
  b = *(float *)(j);
  if (a > b) return (1);
  if (a < b) return (-1);
  return(0);
}

/* shuffle points into random order */
void shuffle(Points *points)
{
  long i, j;
  Point temp;
  for (i=0;i<points->num-1;i++) {
    j=(lrand48()%(points->num - i)) + i;
    temp = points->p[i];
    points->p[i] = points->p[j];
    points->p[j] = temp;
  }
}

/* shuffle an array of integers */
void intshuffle(int *intarray, int length)
{
  long i, j;
  int temp;
  for (i=0;i<length;i++) {
    j=(lrand48()%(length - i))+i;
    temp = intarray[i];
    intarray[i]=intarray[j];
    intarray[j]=temp;
  }
}

/* compute Euclidean distance squared between two points */
float dist(Point p1, Point p2, int dim )
{
  cnt_dist++;
  
#ifdef USE_RISCV_VECTOR
  float result=0.0;
  int i;

#ifdef INTRINSICS
  unsigned long int gvl = _MMR_VSETVL_E32M1(dim);

 _MMR_f32 result1,result2, _aux, _diff, _coord1, _coord2;

  result1 = _MM_SET_f32(0.0,gvl);
  result2 = _MM_SET_f32(0.0,gvl);
  for (i=0;i<dim;i=i+gvl) {

    gvl = _MMR_VSETVL_E32M1(dim-i);

    _coord1 = _MM_LOAD_f32(&(p1.coord[i]),gvl);
    _coord2 = _MM_LOAD_f32(&(p2.coord[i]),gvl);

    _diff = _MM_SUB_f32(_coord2,_coord1,gvl);
    result1   = _MM_MACC_f32(result1,_diff,_diff,gvl);
  }
  result2 = _MM_REDSUM_f32(result1,result2,gvl);
  result = _MM_VGETFIRST_f32(result2);
#else

  unsigned long int gvl;

  asm volatile ("vsetvli %0, %1, e32, m4, ta, ma" : "=r"(gvl) : "r"(dim));
  asm volatile ("vmv.v.x v4, zero");
  asm volatile ("vmv.v.x v24, zero");
  
  for (i=0;i<dim;i=i+2*gvl) {
    // First iteration
    asm volatile ("vsetvli %0, %1, e32, m4, ta, ma" : "=r"(gvl) : "r"(dim-i));
    asm volatile ("vle32.v v12, (%0)"::"r"(&(p1.coord[i])));
    asm volatile ("vle32.v v16, (%0)"::"r"(&(p2.coord[i])));
    asm volatile ("vfsub.vv v20, v12, v16");
    asm volatile ("vfmacc.vv v24, v20, v20");
    int j= i + gvl;
    
    // Exit check: only do second iteration if there's more data
    if (j >= dim) break;
    
    // Second iteration with v8, v28 for loads
    asm volatile ("vsetvli %0, %1, e32, m4, ta, ma" : "=r"(gvl) : "r"(dim-j));
    asm volatile ("vle32.v v8, (%0)"::"r"(&(p1.coord[j])));
    asm volatile ("vle32.v v28, (%0)"::"r"(&(p2.coord[j])));
    asm volatile ("vfsub.vv v20, v8, v28");
    asm volatile ("vfmacc.vv v24, v20, v20");
  }
  asm volatile ("vfredusum.vs v4, v24, v4");
  asm volatile ("vfmv.f.s %0, v4":"=f"(result));

#endif
  return result;
#else // USE_RISCV_VECTOR
  int i;
  float result=0.0;
  for (i=0;i<dim;i++)
    result += (p1.coord[i] - p2.coord[i])*(p1.coord[i] - p2.coord[i]);

  return(result);

#endif // USE_RISCV_VECTOR

}

#ifdef TBB_VERSION

#else //!TBB_VERSION

static double costs[nproc];
float pspeedy(Points *points, float z, long *kcenter, int pid, pthread_barrier_t* barrier)
{
#ifdef ENABLE_THREADS
  pthread_barrier_wait(barrier);
#endif
  //my block
  long bsize = points->num/nproc;
  long k1 = bsize * pid;
  long k2 = k1 + bsize;
  if( pid == nproc-1 )
    k2 = points->num;

  static double totalcost;

  static bool open = false;
  static int i;

#ifdef ENABLE_THREADS
  static pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;
  static pthread_cond_t cond = PTHREAD_COND_INITIALIZER;
#endif

  /* create center at first point, send it to itself */
  for( int k = k1; k < k2; k++ )    {
    float distance = dist(points->p[k],points->p[0],points->dim );
    points->p[k].cost = distance * points->p[k].weight;
    points->p[k].assign=0;
  }

  if( pid==0 ) {
    *kcenter = 1;
  }

#ifdef ENABLE_THREADS
  pthread_barrier_wait(barrier);
#endif

  if( pid != 0 ) { // we are not the master threads. we wait until a center is opened.
    while(1) {
#ifdef ENABLE_THREADS
      pthread_mutex_lock(&mutex);
      while(!open) pthread_cond_wait(&cond,&mutex);
      pthread_mutex_unlock(&mutex);
#endif
      if( i >= points->num ) 
        break;
        
      for( int k = k1; k < k2; k++ ) {
        float distance = dist(points->p[i],points->p[k],points->dim);
        if( distance*points->p[k].weight < points->p[k].cost ) {
          points->p[k].cost = distance * points->p[k].weight;
          points->p[k].assign=i;
        }
      }
#ifdef ENABLE_THREADS
      pthread_barrier_wait(barrier);
      pthread_barrier_wait(barrier);
#endif
    } // while
  } // if not master thread
  else  { // I am the master thread. I decide whether to open a center and notify others if so.
    // printf("open a center %d...\n", points->num);
    for(i = 1; i < points->num; i++ )  {
      // printf("checking point %d...cost: %lf\n",i, points->p[i].cost);
      bool to_open = ((float)lrand48()/(float)INT_MAX)<(points->p[i].cost/z);
      if( to_open )  {
        (*kcenter)++;

#ifdef ENABLE_THREADS
  pthread_mutex_lock(&mutex);
#endif
        open = true;

#ifdef ENABLE_THREADS
  pthread_mutex_unlock(&mutex);
  pthread_cond_broadcast(&cond);
#endif

        for( int k = k1; k < k2; k++ )  {
          float distance = dist(points->p[i],points->p[k],points->dim );
          if( distance*points->p[k].weight < points->p[k].cost )  {
            points->p[k].cost = distance * points->p[k].weight;
            points->p[k].assign=i;
            // printf("point %d: assign to %d\n",k, i);
          }
        }

#ifdef ENABLE_THREADS
        pthread_barrier_wait(barrier);
#endif
        open = false;

#ifdef ENABLE_THREADS
        pthread_barrier_wait(barrier);
#endif
      }
    }
#ifdef ENABLE_THREADS
    pthread_mutex_lock(&mutex);
#endif
    open = true;
#ifdef ENABLE_THREADS
    pthread_mutex_unlock(&mutex);
    pthread_cond_broadcast(&cond);
#endif
  }
#ifdef ENABLE_THREADS
  pthread_barrier_wait(barrier);
#endif
  open = false;
  double mytotal = 0;
  for( int k = k1; k < k2; k++ )  {
    mytotal += points->p[k].cost;
  }
  costs[pid] = mytotal;
#ifdef ENABLE_THREADS
  pthread_barrier_wait(barrier);
#endif
  // aggregate costs from each thread
  if( pid == 0 ) {
    totalcost=z*(*kcenter);
    for( int i = 0; i < nproc; i++ ) {
      totalcost += costs[i];
    }
  }
#ifdef ENABLE_THREADS
  pthread_barrier_wait(barrier);
#endif

  return(totalcost);
}

#endif // TBB_VERSION


/* For a given point x, find the cost of the following operation:
 * -- open a facility at x if there isn't already one there,
 * -- for points y such that the assignment distance of y exceeds dist(y, x),
 *    make y a member of x,
 * -- for facilities y such that reassigning y and all its members to x
 *    would save cost, realize this closing and reassignment.
 *
 * If the cost of this operation is negative (i.e., if this entire operation
 * saves cost), perform this operation and return the amount of cost saved;
 * otherwise, do nothing.
 */

/* numcenters will be updated to reflect the new number of centers */
/* z is the facility cost, x is the number of this point in the array
   points */


#ifdef TBB_VERSION

#else //!TBB_VERSION


double pgain(long x, Points *points, double z, long int *numcenters, int pid, pthread_barrier_t* barrier)
{
  
  // printf("ppgain pthread %d feasible=%lu z=%lf num centers=%d begin\n",pid, x, z, *numcenters);
#ifdef ENABLE_THREADS
  pthread_barrier_wait(barrier);
#endif

  //my block
  long bsize = points->num/nproc;
  long k1 = bsize * pid;
  long k2 = k1 + bsize;
  if( pid == nproc-1 ) k2 = points->num;

  int i;
  int number_of_centers_to_close = 0;

  static double *work_mem;
  static double gl_cost_of_opening_x;
  static int gl_number_of_centers_to_close;

  //each thread takes a block of working_mem.
  int stride = *numcenters+2;
  //make stride a multiple of CACHE_LINE
  int cl = CACHE_LINE/sizeof(double);
  if( stride % cl != 0 ) {
    stride = cl * ( stride / cl + 1);
  }
  int K = stride -2 ; // K==*numcenters

  // printf("ppgain: stride=%d K=%d\n", stride, K);

  //my own cost of opening x
  double cost_of_opening_x = 0;

  if( pid==0 ) {
    work_mem = (double*) baremetal_malloc(stride*(nproc+1)*sizeof(double));
    memset(work_mem, 0, stride*(nproc+1)*sizeof(double));
    gl_cost_of_opening_x = 0;
    gl_number_of_centers_to_close = 0;
  }

#ifdef ENABLE_THREADS
  pthread_barrier_wait(barrier);
#endif
  /*For each center, we have a *lower* field that indicates
    how much we will save by closing the center.
    Each thread has its own copy of the *lower* fields as an array.
    We first build a table to index the positions of the *lower* fields.
  */

  int count = 0;
  for( int i = k1; i < k2; i++ ) {
    if( is_center[i] ) {
      // printf("ppgain: center found at %d\n", i);
      center_table[i] = count++;
    }
  }
  work_mem[pid*stride] = count;

#ifdef ENABLE_THREADS
  pthread_barrier_wait(barrier);
#endif

  if( pid == 0 ) {
    int accum = 0;
    for( int p = 0; p < nproc; p++ ) {
      int tmp = (int)work_mem[p*stride];
      work_mem[p*stride] = accum;
      accum += tmp;
    }
    // printf("accum: %d\n", accum);
  }

#ifdef ENABLE_THREADS
  pthread_barrier_wait(barrier);
#endif

  for( int i = k1; i < k2; i++ ) {
    if( is_center[i] ) {
      center_table[i] += (int)work_mem[pid*stride];
    }
  }

  // printf("--------ppgain: center table built-----\n");
  // printf("count: %d\n", count);

  //now we finish building the table. clear the working memory.
  // printf("ppgain: clear working memory...\n");
  memset(switch_membership + k1, 0, (k2-k1)*sizeof(bool));
  memset(work_mem+pid*stride, 0, stride*sizeof(double));
  if( pid== 0 )
    memset(work_mem+nproc*stride,0,stride*sizeof(double));

#ifdef ENABLE_THREADS
  pthread_barrier_wait(barrier);
#endif

  // printf("after memset: %d\n", (int)work_mem[pid*stride]);

  //my *lower* fields
  double* lower = &work_mem[pid*stride];
  //global *lower* fields
  double* gl_lower = &work_mem[nproc*stride];

  // printf("----------------------------------------------------\n");
  for ( i = k1; i < k2; i++ ) {
    
    float x_cost = dist(points->p[i], points->p[x], points->dim) * points->p[i].weight;
    float current_cost = points->p[i].cost;
    // printf("dim = %d x_cost = %f current_cost = %f\n" , points->dim, x_cost, current_cost);

    if ( x_cost < current_cost ) {

      // point i would save cost just by switching to x
      // (note that i cannot be a median,
      // or else dist(p[i], p[x]) would be 0)

      switch_membership[i] = 1;
      cost_of_opening_x += x_cost - current_cost;
      // printf("ppgain: switch_membership[%d]=true cost_of_opening_x=%lf\n", i, cost_of_opening_x);
    } else {

      // cost of assigning i to x is at least current assignment cost of i

      // consider the savings that i's **current** median would realize
      // if we reassigned that median and all its members to x;
      // note we've already accounted for the fact that the median
      // would save z by closing; now we have to subtract from the savings
      // the extra cost of reassigning that median and its members
      int assign = points->p[i].assign;
      lower[center_table[assign]] += current_cost - x_cost;
      // printf("ppgain: lower[%d]=%lf\n",center_table[assign], lower[center_table[assign]]);
    }
  }

#ifdef ENABLE_THREADS
  pthread_barrier_wait(barrier);
#endif

  // at this time, we can calculate the cost of opening a center
  // at x; if it is negative, we'll go through with opening it

  for ( int i = k1; i < k2; i++ ) {
    if( is_center[i] ) {
      double low = z;
      //aggregate from all threads
      for( int p = 0; p < nproc; p++ ) {
        low += work_mem[center_table[i]+p*stride];
      }
      gl_lower[center_table[i]] = low;
      if ( low > 0 ) {
        // i is a median, and
        // if we were to open x (which we still may not) we'd close i

        // note, we'll ignore the following quantity unless we do open x
        ++number_of_centers_to_close;
        cost_of_opening_x -= low;
      }
    }
  }

  //use the rest of working memory to store the following
  work_mem[pid*stride + K] = number_of_centers_to_close;
  work_mem[pid*stride + K+1] = cost_of_opening_x;

  // printf("ppgain: cost of opening x = %lf number of centers to close = %d\n", cost_of_opening_x, number_of_centers_to_close);

#ifdef ENABLE_THREADS
  pthread_barrier_wait(barrier);
#endif
  //  printf("thread %d cost complete\n",pid);

  if( pid==0 ) {
    gl_cost_of_opening_x = z;
    //aggregate
    for( int p = 0; p < nproc; p++ ) {
      gl_number_of_centers_to_close += (int)work_mem[p*stride + K];
      gl_cost_of_opening_x += work_mem[p*stride+K+1];
    }
  }
#ifdef ENABLE_THREADS
  pthread_barrier_wait(barrier);
#endif
  // Now, check whether opening x would save cost; if so, do it, and
  // otherwise do nothing

  if ( gl_cost_of_opening_x < 0 ) {
    // printf("Opening a new center at %d would save cost %lf by closing %d centers\n", x, gl_cost_of_opening_x, (int)gl_number_of_centers_to_close);
    //  we'd save money by opening x; we'll do it
    for ( int i = k1; i < k2; i++ ) {
      bool close_center = gl_lower[center_table[points->p[i].assign]] > 0 ;
      if ( switch_membership[i] || close_center ) {
        // Either i's median (which may be i itself) is closing,
        // or i is closer to x than to its current median
        points->p[i].cost = points->p[i].weight *
          dist(points->p[i], points->p[x], points->dim );
        points->p[i].assign = x;
      }
    }
    for( int i = k1; i < k2; i++ ) {
      if( is_center[i] && gl_lower[center_table[i]] > 0 ) {
        is_center[i] = false;
      }
    }
    if( x >= k1 && x < k2 ) {
      is_center[x] = true;
    }

    if( pid==0 ) {
      *numcenters = *numcenters + 1 - gl_number_of_centers_to_close;
    }
  }
  else {
    if( pid==0 )
      gl_cost_of_opening_x = 0;  // the value we'll return
  }
#ifdef ENABLE_THREADS
  pthread_barrier_wait(barrier);
#endif

  return -gl_cost_of_opening_x;
}

#endif // TBB_VERSION



/* facility location on the points using local search */
/* z is the facility cost, returns the total cost and # of centers */
/* assumes we are seeded with a reasonable solution */
/* cost should represent this solution's cost */
/* halt if there is < e improvement after iter calls to gain */
/* feasible is an array of numfeasible points which may be centers */

#ifdef TBB_VERSION

#else //!TBB_VERSION
 float pFL(Points *points, int *feasible, int numfeasible,
    float z, long *k, double cost, long iter, float e,
    int pid, pthread_barrier_t* barrier)
{
  // printf("----------------pFL----------------\n");
#ifdef ENABLE_THREADS
  pthread_barrier_wait(barrier);
#endif
  long i;
  long x;
  double change;

  change = cost;
  /* continue until we run iter iterations without improvement */
  /* stop instead if improvement is less than e */
  // printf("z=%f cost=%lf iter=%d\n", z, cost, iter);
  while (change/cost > 1.0*e) {
    
    // printf("metric:%lf ref:%lf\n",change/cost, 1.0*e);
    change = 0.0;
    /* randomize order in which centers are considered */

    if( pid == 0 ) {
      intshuffle(feasible, numfeasible);
    }
#ifdef ENABLE_THREADS
    pthread_barrier_wait(barrier);
#endif
    for (i=0;i<iter;i++) {
      x = i%numfeasible;
      // printf("ITER:%d considering point %d...\n", i, feasible[x]);
      change += pgain(feasible[x], points, z, k, pid, barrier);
    }
    cost -= change;
#ifdef ENABLE_THREADS
    pthread_barrier_wait(barrier);
#endif
  }
  return(cost);
}

#endif // TBB_VERSION

#ifdef TBB_VERSION
int selectfeasible_fast(Points *points, int **feasible, int kmin)
#else
int selectfeasible_fast(Points *points, int **feasible, int kmin, int pid, pthread_barrier_t* barrier)
#endif
{
  // printf("selecting feasible centers...%d\n", points->num);
  int numfeasible = points->num;
  if (numfeasible > (ITER*kmin*log((double)kmin)))
    numfeasible = (int)(ITER*kmin*log((double)kmin));
  *feasible = (int *)baremetal_malloc(numfeasible*sizeof(int));
  // printf("num feasible centers: %d\n", numfeasible);

  float* accumweight;
  float totalweight;

  /*
     Calcuate my block.
     For now this routine does not seem to be the bottleneck, so it is not parallelized.
     When necessary, this can be parallelized by setting k1 and k2 to
     proper values and calling this routine from all threads ( it is called only
     by thread 0 for now ).
     Note that when parallelized, the randomization might not be the same and it might
     not be difficult to measure the parallel speed-up for the whole program.
   */
  //  long bsize = numfeasible;
  long k1 = 0;
  long k2 = numfeasible;

  float w;
  int l,r,k;

  /* not many points, all will be feasible */
  if (numfeasible == points->num) {
    for (int i=k1;i<k2;i++)
      (*feasible)[i] = i;
    return numfeasible;
  }
#ifdef TBB_VERSION
  accumweight= (float*)memoryFloat.allocate(sizeof(float)*points->num);
#else
  accumweight= (float*)baremetal_malloc(sizeof(float)*points->num);
#endif

  accumweight[0] = points->p[0].weight;
  totalweight=0;
  for( int i = 1; i < points->num; i++ ) {
    accumweight[i] = accumweight[i-1] + points->p[i].weight;
  }
  totalweight=accumweight[points->num-1];

  for(int i=k1; i<k2; i++ ) {
    w = (lrand48()/(float)INT_MAX)*totalweight;
    //binary search
    l=0;
    r=points->num-1;
    if( accumweight[0] > w )  {
      (*feasible)[i]=0;
      continue;
    }
    while( l+1 < r ) {
      k = (l+r)/2;
      if( accumweight[k] > w ) {
        r = k;
      } else {
        l=k;
      }
    }
    (*feasible)[i]=r;
  }

  // printf("selecting feasible centers...done\n");
  for (int i=k1;i<k2;i++) {
    // printf("feasible[%d]=%d\n",i,(*feasible)[i]);
  }

#ifdef TBB_VERSION
  memoryFloat.deallocate(accumweight, sizeof(float));
#else
  // free(accumweight);
#endif

  return numfeasible;
}



#ifdef TBB_VERSION


#else //!TBB_VERSION

static double hizs[nproc];

/* compute approximate kmedian on the points */
float pkmedian(Points *points, long kmin, long kmax, long* kfinal,
         int pid, pthread_barrier_t* barrier )
{
  printf("pkmedian pthread %d begin\n",pid);
  
  start_timer();
  
  int i;
  double cost;
  double hiz, loz, z;

  static long k;
  static int *feasible;
  static int numfeasible;

  hiz = loz = 0.0;
  long ptDimension = points->dim;

  //my block
  long bsize = points->num/nproc;
  long k1 = bsize * pid;
  long k2 = k1 + bsize;
  if( pid == nproc-1 ) k2 = points->num;

#ifdef ENABLE_THREADS
  pthread_barrier_wait(barrier);
#endif

  double myhiz = 0;
  for (long kk=k1;kk < k2; kk++ ) {
    myhiz += dist(points->p[kk], points->p[0],
          ptDimension )*points->p[kk].weight;
  }
  hizs[pid] = myhiz;

#ifdef ENABLE_THREADS
  pthread_barrier_wait(barrier);
#endif

  for( int i = 0; i < nproc; i++ )   {
    hiz += hizs[i];
  }

  loz=0.0; 
  z = (hiz+loz)/2.0;
  /* NEW: Check whether more centers than points! */
  if (points->num <= kmax) {
    /* just return all points as facilities */
    for (long kk=k1;kk<k2;kk++) {
      points->p[kk].assign = kk;
      points->p[kk].cost = 0;
    }
    cost = 0;
    if( pid== 0 ) {
      *kfinal = k;
    }
    return cost;
  }

  if( pid == 0 )
    shuffle(points);

  cost = pspeedy(points, z, &k, pid, barrier);
  
  i=0;

  /* give speedy SP chances to get at least kmin/2 facilities */
  while ((k < kmin)&&(i<SP)) {
    cost = pspeedy(points, z, &k, pid, barrier);
    i++;
  }

  /* if still not enough facilities, assume z is too high */
  while (k < kmin) {
    if (i >= SP) {hiz=z; z=(hiz+loz)/2.0; i=0;}
    if( pid == 0 ) shuffle(points);
    cost = pspeedy(points, z, &k, pid, barrier);
    i++;
  }

  // printf("pspeedy...%lf, num open centers=%d\n", cost, k);

  /* now we begin the binary search for real */
  /* must designate some points as feasible centers */
  /* this creates more consistancy between FL runs */
  /* helps to guarantee correct # of centers at the end */

  if( pid == 0 ){
    numfeasible = selectfeasible_fast(points,&feasible,kmin,pid,barrier);
    for( int i = 0; i< points->num; i++ ) {
      is_center[points->p[i].assign]= true;
    }
  }

#ifdef ENABLE_THREADS
  pthread_barrier_wait(barrier);
#endif

  stop_timer();
  printf("pspeedy [sw-cycles]: %ld cnt: %d\n", get_timer(), cnt_dist);
  // This is the first start_timer()/stop_timer() bracket the program
  // reaches (localSearch's very first pkmedian call), matching the region
  // the RTL harness's status.csv "ROI_Cycles" column measures for this
  // benchmark - unlike axpy/etc where the harness's first-bracket and
  // whole-kernel regions coincide, streamcluster has several distinct
  // phase timers and this is specifically the first one.
  printf("[ROI-LATENCY]: %ld cycles\n", get_timer());

  start_timer();
  while(1) {
    /* first get a rough estimate on the FL solution */
    cost = pFL(points, feasible, numfeasible,
                z, &k, cost, (long)(ITER*kmax*log((double)kmax)), 0.1, pid, barrier);

    /* if number of centers seems good, try a more accurate FL */
    if (((k <= (1.1)*kmax)&&(k >= (0.9)*kmin))|| ((k <= kmax+2)&&(k >= kmin-2))) {

      /* may need to run a little longer here before halting without
      improvement */
      cost = pFL(points, feasible, numfeasible,
                  z, &k, cost, (long)(ITER*kmax*log((double)kmax)), 0.001, pid, barrier);
    }

    if (k > kmax) {
      /* facilities too cheap */
      /* increase facility cost and up the cost accordingly */
      loz = z; 
      z = (hiz+loz)/2.0;
      cost += (z-loz)*k;
    }
    if (k < kmin) {
      /* facilities too expensive */
      /* decrease facility cost and reduce the cost accordingly */
      hiz = z; 
      z = (hiz+loz)/2.0;
      cost += (z-hiz)*k;
    }

    /* if k is good, return the result */
    /* if we're stuck, just give up and return what we have */
    if (((k <= kmax)&&(k >= kmin))||((loz >= (0.999)*hiz)) ) {
      break;
    }
#ifdef ENABLE_THREADS
    pthread_barrier_wait(barrier);
#endif
  }
  stop_timer();
  printf("pFL [sw-cycles]: %ld cnt:%d \n", get_timer(), cnt_dist);

  //clean up...
  if( pid==0 ) {
    *kfinal = k;
  }

  return cost;
}

#endif // TBB_VERSION




/* compute the means for the k clusters */
int contcenters(Points *points)
{
  long i, ii;
  float relweight;

  for (i=0;i<points->num;i++) {
    /* compute relative weight of this point to the cluster */
    if (points->p[i].assign != i) {
      relweight=points->p[points->p[i].assign].weight + points->p[i].weight;
      relweight = points->p[i].weight/relweight;
      for (ii=0;ii<points->dim;ii++) {
        points->p[points->p[i].assign].coord[ii] *= 1.0-relweight;
        points->p[points->p[i].assign].coord[ii] += points->p[i].coord[ii]*relweight;
      }
      points->p[points->p[i].assign].weight += points->p[i].weight;
    }
  }

  return 0;
}

/* copy centers from points to centers */
void copycenters(Points *points, Points* centers, long* centerIDs, long offset)
{
  long i;
  long k;

  bool *is_a_median = (bool *) baremetal_malloc(points->num * sizeof(bool));
  memset(is_a_median, 0, points->num * sizeof(bool));

  /* mark the centers */
  for ( i = 0; i < points->num; i++ ) {
    is_a_median[points->p[i].assign] = 1;
  }

  k=centers->num;

  /* count how many  */
  for ( i = 0; i < points->num; i++ ) {
    if ( is_a_median[i] ) {
      memcpy( centers->p[k].coord, points->p[i].coord, points->dim * sizeof(float));
      centers->p[k].weight = points->p[i].weight;
      centerIDs[k] = i + offset;
      k++;
    }
  }

  centers->num = k;

  // free(is_a_median);
}

struct pkmedian_arg_t
{
  Points* points;
  long kmin;
  long kmax;
  long* kfinal;
  int pid;
  pthread_barrier_t* barrier;
};

void* localSearchSub(void* arg_) {

  pkmedian_arg_t* arg= (pkmedian_arg_t*)arg_;
  pkmedian(arg->points,arg->kmin,arg->kmax,arg->kfinal,arg->pid,arg->barrier);

  return NULL;
}

#ifdef TBB_VERSION

#else //!TBB_VERSION

pkmedian_arg_t arg[nproc];
void localSearch( Points* points, long kmin, long kmax, long* kfinal ) {
    pthread_barrier_t barrier;
    // pthread_t* threads = new pthread_t[nproc];

#ifdef ENABLE_THREADS
    pthread_barrier_init(&barrier,NULL,nproc);
#endif
    for( int i = 0; i < nproc; i++ ) {
      arg[i].points = points;
      arg[i].kmin = kmin;
      arg[i].kmax = kmax;
      arg[i].pid = i;
      arg[i].kfinal = kfinal;

      arg[i].barrier = &barrier;
#ifdef ENABLE_THREADS
      pthread_create(threads+i,NULL,localSearchSub,(void*)&arg[i]);
#else
      localSearchSub(&arg[0]);
#endif
    }

#ifdef ENABLE_THREADS
    for ( int i = 0; i < nproc; i++) {
      pthread_join(threads[i],NULL);
    }
#endif

    // delete[] threads;
    // delete[] arg;
#ifdef ENABLE_THREADS
    pthread_barrier_destroy(&barrier);
#endif
}
#endif // TBB_VERSION

void outcenterIDs( Points* centers, long* centerIDs) {
  int* is_a_median = (int*)baremetal_malloc( centers->num * sizeof(int) );
  memset(is_a_median, 0, centers->num * sizeof(int));

  for( int i =0 ; i< centers->num; i++ ) {
    is_a_median[centers->p[i].assign] = 1;
  }

  for( int i = 0; i < centers->num; i++ ) {
    if( is_a_median[i] ) {
      printf("%lu\n", centerIDs[i]);
      printf("%f\n", centers->p[i].weight);
      for( int k = 0; k < centers->dim; k++ ) {
        printf("%f ", centers->p[i].coord[k]);
      }
      printf("\n\n");
    }
  }
}

void streamCluster(long kmin, long kmax, int dim,
        long chunksize, long centersize)
{

#ifdef TBB_VERSION
  float* block = (float*)memoryFloat.allocate( chunksize*dim*sizeof(float) );
  float* centerBlock = (float*)memoryFloat.allocate(centersize*dim*sizeof(float) );
  long* centerIDs = (long*)memoryLong.allocate(centersize*dim*sizeof(long));
#else
  // float* block = (float*)baremetal_malloc( chunksize*dim*sizeof(float) );
  float* centerBlock = (float*)baremetal_malloc(centersize*dim*sizeof(float) );
  long* centerIDs = (long*)baremetal_malloc(centersize*dim*sizeof(long));
#endif

  Points points;
  points.dim = dim;
  points.num = chunksize;
  points.p =
#ifdef TBB_VERSION
    (Point *)memoryPoint.allocate(chunksize*sizeof(Point), NULL);
#else
    (Point *)baremetal_malloc(chunksize*sizeof(Point));
#endif

  for( int i = 0; i < chunksize; i++ ) {
    points.p[i].coord = &block[i*dim];
  }

  Points centers;
  centers.dim = dim;
  centers.p =
#ifdef TBB_VERSION
    (Point *)memoryPoint.allocate(centersize*sizeof(Point), NULL);
#else
    (Point *)baremetal_malloc(centersize*sizeof(Point));
#endif
  centers.num = 0;

  for( int i = 0; i< centersize; i++ ) {
    centers.p[i].coord = &centerBlock[i*dim];
    centers.p[i].weight = 1.0;
  }

  long IDoffset = 0;
  long kfinal;

  while(1) {

    for( int i = 0; i < points.num; i++ ) {
      points.p[i].weight = 1.0;
    }

#ifdef TBB_VERSION
    switch_membership = (bool*)memoryBool.allocate(points.num*sizeof(bool), NULL);
    is_center = (bool*)calloc(points.num,sizeof(bool));
    center_table = (int*)memoryInt.allocate(points.num*sizeof(int));
#else
    switch_membership = (bool*)baremetal_malloc(points.num*sizeof(bool));
    is_center = (bool*)baremetal_malloc(points.num*sizeof(bool));
    memset(is_center, 0, points.num*sizeof(bool));
    center_table = (int*)baremetal_malloc(points.num*sizeof(int));
#endif
    
    localSearch(&points, kmin, kmax, &kfinal); // parallel

    start_timer();
    contcenters(&points); /* sequential */

    if( kfinal + centers.num > centersize ) {
      // here we don't handle the situation where # of centers gets too large.
      printf("oops! no more space for centers\n");
      exit(1);
    }

    copycenters(&points, &centers, centerIDs, IDoffset); /* sequential */
    IDoffset += chunksize;

#ifdef TBB_VERSION
    memoryBool.deallocate(switch_membership, sizeof(bool));
    free(is_center);
    memoryInt.deallocate(center_table, sizeof(int));
#else
#endif

    // TODO: add check to exit loop when no more data to read
    // For now just break
    break;

    stop_timer();
    printf("cont & copy centers [sw-cycles]: %ld cnt:%d\n", get_timer(), cnt_dist);

  }

  //finally cluster all temp centers
#ifdef TBB_VERSION
  switch_membership = (bool*)memoryBool.allocate(centers.num*sizeof(bool));
  is_center = (bool*)calloc(centers.num,sizeof(bool));
  center_table = (int*)memoryInt.allocate(centers.num*sizeof(int));
#else
  switch_membership = (bool*)baremetal_malloc(centers.num*sizeof(bool));
  is_center = (bool*)baremetal_malloc(centers.num*sizeof(bool));
  memset(is_center, 0, centers.num*sizeof(bool));
  center_table = (int*)baremetal_malloc(centers.num*sizeof(int));
#endif

  localSearch( &centers, kmin, kmax ,&kfinal ); // parallel

  start_timer();
  contcenters(&centers);
  stop_timer();
  printf("cont centers [sw-cycles]: %ld cnt:%d\n", get_timer(), cnt_dist);
  
  outcenterIDs( &centers, centerIDs);
}

int main()
{
  long kmin, kmax, n, chunksize, clustersize;
  int dim;

#ifdef PARSEC_VERSION
#define __PARSEC_STRING(x) #x
#define __PARSEC_XSTRING(x) __PARSEC_STRING(x)
        fprintf(stderr,"PARSEC Benchmark Suite Version "__PARSEC_XSTRING(PARSEC_VERSION)"\n");
  fflush(NULL);
#else
#endif //PARSEC_VERSION
#ifdef ENABLE_PARSEC_HOOKS
  __parsec_bench_begin(__parsec_streamcluster);
#endif

//sim large : 10 20 128 4096 4096 1000 none output_vec

  kmin = KMIN;
  kmax = KMAX;
  dim = DIM;
  n = N;
  chunksize = CHUNKSIZE;
  clustersize = CLUSTERSIZE;

  printf("kmin = %ld, kmax = %ld, dim = %d, n = %ld, chunksize = %ld, clustersize = %ld\n",
         kmin, kmax, dim, n, chunksize, clustersize);

#ifdef TBB_VERSION
  fprintf(stderr,"TBB version. Number of divisions: %d\n",NUM_DIVISIONS);
  tbb::task_scheduler_init init(nproc);
#endif

  srand48(SEED);

#ifdef ENABLE_PARSEC_HOOKS
  __parsec_roi_begin();
#endif

  printf("Running StreamCluster with %d threads L=%d C=%d\n", nproc, NR_LANES, NR_CLUSTERS);

  HW_CNT_READY;
  int64_t e2e_start_cycles = get_cycle_count();
  // Not using start_timer()/stop_timer()/get_timer() here: those share one
  // global `timer` that streamCluster()'s own nested per-phase timing
  // (pkmedian's start_timer/stop_timer calls) would reset partway through,
  // clobbering a whole-run measurement taken the same way. Read the cycle
  // counter directly instead so this ROI window is independent of that.
  int64_t roi_start_cycles = get_cycle_count();

  streamCluster(kmin, kmax, dim, chunksize, clustersize);

  asm volatile("fence");
  int64_t roi_cycles = get_cycle_count() - roi_start_cycles;
  // Not "[ROI-LATENCY]" - that tag is reserved for the pspeedy print
  // above, which is what's actually comparable to the RTL baseline's
  // ROI_Cycles figure for this benchmark. This is the whole streamCluster()
  // call (all ~1200 refinement iterations), informative but not the same
  // measurement.
  printf("[WHOLE-KERNEL-LATENCY]: %ld cycles\n", roi_cycles);
  int64_t e2e_total_cycles = get_cycle_count() - e2e_start_cycles;
  printf("[E2E-LATENCY]: %ld cycles\n", e2e_total_cycles);

  printf("Number of distance calculations: %d\n", cnt_dist);

#ifdef ENABLE_PARSEC_HOOKS
  __parsec_roi_end();
#endif

#ifdef ENABLE_PARSEC_HOOKS
  __parsec_bench_end();
#endif

  return 0;
}
