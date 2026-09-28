// main.cpp
//
// Created by Daniel Schwartz-Narbonne on 13/04/07.
// Modified by Christian Bienia
//
// Copyright 2007-2008 Princeton University
// All rights reserved.
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions
// are met:
// 1. Redistributions of source code must retain the above copyright
//    notice, this list of conditions and the following disclaimer.
// 2. Redistributions in binary form must reproduce the above copyright
//    notice, this list of conditions and the following disclaimer in the
//    documentation and/or other materials provided with the distribution.
//
// THIS SOFTWARE IS PROVIDED BY THE AUTHOR AND CONTRIBUTORS ``AS IS'' AND
// ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
// IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
// ARE DISCLAIMED.  IN NO EVENT SHALL THE AUTHOR OR CONTRIBUTORS BE LIABLE
// FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
// DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
// OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
// HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
// LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
// OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
// SUCH DAMAGE.

/*************************************************************************
* RISC-V Vectorized Version
* Author: Cristóbal Ramírez Lazo
* email: cristobal.ramirez@bsc.es
* Barcelona Supercomputing Center (2020)
*************************************************************************/

#ifdef USE_RISCV_VECTOR
#include <riscv_vector.h>
#include "common/vector_defines.h"
#endif

#ifdef ENABLE_THREADS
#include <pthread.h>
#endif

#ifdef ENABLE_PARSEC_HOOKS
#include <hooks.h>
#endif

#include "annealer_types.h"
#include "annealer_thread.h"
#include "netlist.h"
#include "rng.h"

#include <stdlib.h>
#include "printf.h"
#include "runtime.h"

// Static compile-time configuration for CANNEAL
#define USE_COMPILED_NETLIST
#define CANNEAL_NUM_THREADS 1
#define CANNEAL_SWAPS_PER_TEMP 10
#define CANNEAL_START_TEMP 2000
#define CANNEAL_NUM_TEMP_STEPS 10  // -1 means run until convergence

void* entry_pt(void*);

int main (int argc, char **argv) {

	// Baremetal execution - no stdout or system time available
	int64_t e2e_start_cycles = get_cycle_count();

#ifdef ENABLE_PARSEC_HOOKS
	__parsec_bench_begin(__parsec_canneal);
#endif

	// srandom(3);

	// Use statically defined configuration values (no command-line parsing)
	int num_threads = CANNEAL_NUM_THREADS;

#ifndef ENABLE_THREADS
	if (num_threads != 1){
		exit(1);
	}
#endif

	// Use static configuration values
	int swaps_per_temp = CANNEAL_SWAPS_PER_TEMP;
	int start_temp = CANNEAL_START_TEMP;
	int number_temp_steps = CANNEAL_NUM_TEMP_STEPS;

	printf(" CANNEAL Configuration:\n");
	printf("  num_threads: %d\n", num_threads);
	printf("  swaps_per_temp: %d\n", swaps_per_temp);
	printf("  start_temp: %d\n", start_temp);
	printf("  number_temp_steps: %d\n", number_temp_steps);
	printf("  Lanes=%d\n", NR_LANES);
	printf("  Clusters=%d\n", NR_CLUSTERS);


	//now that we've read in the commandline, run the program
	netlist my_netlist(true); // Use compiled static netlist data

	annealer_thread a_thread(&my_netlist,num_threads,swaps_per_temp,start_temp,number_temp_steps);
	
#ifdef ENABLE_PARSEC_HOOKS
	__parsec_roi_begin();
#endif

#ifdef ENABLE_THREADS
	std::vector<pthread_t> threads(num_threads);
	void* thread_in = static_cast<void*>(&a_thread);
	for(int i=0; i<num_threads; i++){
		pthread_create(&threads[i], NULL, entry_pt,thread_in);
	}
	for (int i=0; i<num_threads; i++){
		pthread_join(threads[i], NULL);
	}
#else
	start_timer();
	a_thread.Run();
	stop_timer();
	printf("[ROI-LATENCY]: %ld cycles\n", get_timer());
	printf("Total execution time [sw-cycles]: %ld\n", get_timer());
#endif

#ifdef ENABLE_PARSEC_HOOKS
	__parsec_roi_end();
#endif

#ifdef USE_RISCV_VECTOR
	extern unsigned long swap_cost_vector_calls;
	printf("swap_cost_vector calls: %lu\n", swap_cost_vector_calls);
#endif

	asm volatile("fence");
	int64_t e2e_total_cycles = get_cycle_count() - e2e_start_cycles;
	printf("[E2E-LATENCY]: %ld cycles\n", e2e_total_cycles);

	return 0;

}
/*
void* entry_pt(void* data)
{
	annealer_thread* ptr = static_cast<annealer_thread*>(data);
	ptr->Run();
}
*/