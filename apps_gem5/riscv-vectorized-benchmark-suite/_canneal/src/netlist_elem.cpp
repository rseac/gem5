// netlist_elem.cpp
//
// Created by Daniel Schwartz-Narbonne on 14/04/07.
//
// Copyright 2007 Princeton University
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



#include "annealer_types.h"
#include "location_t.h"
#include "netlist_elem.h"
#include "math.h"

/*************************************************************************
* RISC-V Vectorized Version
* Author: Cristóbal Ramírez Lazo
* email: cristobal.ramirez@bsc.es
* Barcelona Supercomputing Center (2020)
*************************************************************************/

#ifdef USE_RISCV_VECTOR
#include "common/vector_defines.h"
unsigned long swap_cost_vector_calls = 0;
#endif

netlist_elem::netlist_elem()
:present_loc(NULL)//start with the present_loc as nothing at all.  Filled in later by the netlist
{
	fanin_count = 0; // Initialize C-style fanin array counter
	fanout_count = 0; // Initialize C-style fanout array counter
	fan_locs_count = 0; // Initialize C-style fanlocs array counter
}

//*****************************************************************************************
// Calculates the routing cost using the manhatten distance
// I make sure to get the pointer in one operation, and then use it
// SYNC: Do i need to make this an atomic operation?  i.e. are there misaligned memoery issues that can cause this to fail
//       even if I have atomic writes?
//*****************************************************************************************
routing_cost_t netlist_elem::routing_cost_given_loc(location_t loc)
{
	routing_cost_t fanin_cost = 0;
	routing_cost_t fanout_cost = 0;

	for (int i = 0; i< fanin_count; ++i){
		location_t* fanin_loc = fanin[i]->present_loc.Get();
		fanin_cost += fabs(loc.x - fanin_loc->x);
		fanin_cost += fabs(loc.y - fanin_loc->y);
	}

	for (int i = 0; i< fanout_count; ++i){
		location_t* fanout_loc = fanout[i]->present_loc.Get();
		fanout_cost += fabs(loc.x - fanout_loc->x);
		fanout_cost += fabs(loc.y - fanout_loc->y);
	}

	routing_cost_t total_cost = fanin_cost + fanout_cost;
	return total_cost;
}

//*****************************************************************************************
//  Get the cost change of swapping from our present location to a new location
//*****************************************************************************************
#ifdef USE_RISCV_VECTOR
int spill_buffer[32 * NR_LANES * NR_CLUSTERS] __attribute__((aligned(4 * NR_LANES * NR_CLUSTERS)));

#ifdef INTRINSICS
routing_cost_t netlist_elem::swap_cost_vector(_MMR_i32 xOld_loc ,_MMR_i32 xNew_loc ,int fan_size)
{
	swap_cost_vector_calls++;

	routing_cost_t no_swap = 0;
	routing_cost_t yes_swap = 0;

	_MMR_i32 xLoc2;
    _MMR_i32 xNo_Swap_i;
    _MMR_f32 xNo_Swap_aux;
    _MMR_f32 xNo_Swap;
    _MMR_f32 xresult_no_swap;
    _MMR_i32 xYes_Swap_i;
    _MMR_f32 xYes_Swap_aux;
    _MMR_f32 xYes_Swap;
    _MMR_f32 xresult_yes_swap;

    int a_size;
	a_size = fan_size*2;

    unsigned long int gvl     = __riscv_vsetvl_e32m1(a_size);
    xresult_no_swap = _MM_SET_f32(0.0f,gvl);
    xresult_yes_swap = _MM_SET_f32(0.0f,gvl);
    xNo_Swap = _MM_SET_f32(0.0f,gvl);
    xYes_Swap = _MM_SET_f32(0.0f,gvl);

    for(int i=0 ; i<a_size ; i = i + gvl)
    {
        // fan_locs  is a vector which holds the pointers to every input and ouput of the current node,
        // Then by loading this first vector, it is possible to access to the pointers of the current location of each input and output.
        gvl     = __riscv_vsetvl_e64m1((a_size-i)/2);

        _MMR_i64   xLoc;
        xLoc = _MM_LOAD_i64((const long *)&(fan_locs[i/2]),gvl);
        xLoc = _MM_LOAD_INDEX_i64(0,_MM_CAST_u64_i64(xLoc),gvl);
        xLoc = _MM_LOAD_INDEX_i64(0,_MM_CAST_u64_i64(xLoc),gvl);

        // This is a spill intentionally added to avoid an expensive reshuffling operations in AraXL
        _MM_STORE_i64((long *)&spill_buffer[0],xLoc,gvl);

        gvl     = __riscv_vsetvl_e32m1(a_size-i);

        xLoc2   = _MM_LOAD_i32((int *)&spill_buffer[0],gvl);
        //xLoc2           = _MM_CAST_i32_i64(xLoc);

        xNo_Swap_i      = _MM_SUB_i32(xOld_loc,xLoc2,gvl);
        xNo_Swap_aux    = _MM_VFCVT_F_X_f32(xNo_Swap_i,gvl);
        xNo_Swap_aux    = _MM_VFSGNJX_f32(xNo_Swap_aux,xNo_Swap_aux,gvl);

        xYes_Swap_i     = _MM_SUB_i32(xNew_loc,xLoc2,gvl);
        xYes_Swap_aux   = _MM_VFCVT_F_X_f32(xYes_Swap_i,gvl);
        xYes_Swap_aux   = _MM_VFSGNJX_f32(xYes_Swap_aux,xYes_Swap_aux,gvl);

        // The last iteration should add few elements and the tail must be undisturbed.
        xNo_Swap        = _MM_ADD_f32(xNo_Swap,xNo_Swap_aux,gvl);
        xYes_Swap       = _MM_ADD_f32(xYes_Swap,xYes_Swap_aux,gvl);
    }

    gvl     = __riscv_vsetvl_e32m1(a_size);

    xresult_no_swap = _MM_REDSUM_f32(xNo_Swap,xresult_no_swap,gvl);
    no_swap = _MM_VGETFIRST_f32(xresult_no_swap);

    xresult_yes_swap = _MM_REDSUM_f32(xYes_Swap,xresult_yes_swap,gvl);
    yes_swap = _MM_VGETFIRST_f32(xresult_yes_swap);

    return (double)(yes_swap - no_swap);
}
#else
routing_cost_t netlist_elem::swap_cost_vector(int fan_size)
{
    swap_cost_vector_calls++;

    float no_swap = 0;
    float yes_swap = 0;

    int a_size;
    a_size = fan_size*2;

    unsigned long int gvl;
    asm volatile ("vsetvli %0, %1, e32, m1, ta, ma" : "=r"(gvl) : "r"(a_size));
    asm volatile ("vmv.v.i v28, 0"); // xNo_Swap
    asm volatile ("vmv.v.i v30, 0"); // xYes_Swap

    for(int i=0 ; i<a_size ; i = i + gvl) {
        asm volatile ("vsetvli %0, %1, e64, m1, ta, ma" : "=r"(gvl) : "r"((a_size-i)/2));

        asm volatile ("vle64.v v20, (%0)"::"r"(&(fan_locs[i/2]))); // Load the base address of fan_locs
        asm volatile ("vloxei64.v v20, (zero), v20"); // Load the pointer to the current location of each input and output
        asm volatile ("vloxei64.v v20, (zero), v20"); // Load the current location of each input and output

        // This is a spill intentionally added to avoid an expensive reshuffling operations in AraXL
        asm volatile ("vse64.v v20, (%0)"::"r"(&spill_buffer[0])); 
        
        asm volatile ("vsetvli %0, %1, e32, m1, ta, ma" : "=r"(gvl) : "r"(a_size-i));
        
        asm volatile ("vle32.v v8, (%0)"::"r"(&spill_buffer[0])); // Load the current location of each input and output
        asm volatile ("vsub.vv v16, v8, v4");
        asm volatile ("vfcvt.f.x.v v16, v16");
        asm volatile ("vfabs.v v16, v16"); // xNo_Swap_aux

        asm volatile ("vsub.vv v24, v8, v12");
        asm volatile ("vfcvt.f.x.v v24, v24");
        asm volatile ("vfabs.v v24, v24"); // xYes_Swap_aux

        asm volatile ("vfadd.vv v28, v16, v28");
        asm volatile ("vfadd.vv v30, v24, v30");
    }

    asm volatile ("vsetvli %0, %1, e32, m1, ta, ma" : "=r"(gvl) : "r"(a_size));
    asm volatile ("vmv.v.i v16, 0"); 
    asm volatile ("vmv.v.i v24, 0"); 

    asm volatile ("vfredusum.vs	v16, v28, v16"); // xresult_no_swap
    asm volatile ("vfredusum.vs	v24, v30, v24"); // xresult_yes_swap

    asm volatile ("vfmv.f.s %0, v16":"=f"(no_swap));
    asm volatile ("vfmv.f.s %0, v24":"=f"(yes_swap));
    
    return (double)(yes_swap - no_swap);
}
#endif

#else // !USE_RISCV_VECTOR

routing_cost_t netlist_elem::swap_cost(location_t* old_loc, location_t* new_loc)
{
	int fanin_size = fanin_count;
	int fanout_size = fanout_count;

	routing_cost_t no_swap = 0;
	routing_cost_t yes_swap = 0;

	for (int i = 0; i< fanin_size; ++i){
		location_t* fanin_loc = fanin[i]->present_loc.Get();

		no_swap += fabs(old_loc->x - fanin_loc->x);
		no_swap += fabs(old_loc->y - fanin_loc->y);

		yes_swap += fabs(new_loc->x - fanin_loc->x);
		yes_swap += fabs(new_loc->y - fanin_loc->y);
	}

	for (int i = 0; i< fanout_size; ++i){
		location_t* fanout_loc = fanout[i]->present_loc.Get();
		no_swap += fabs(old_loc->x - fanout_loc->x);
		no_swap += fabs(old_loc->y - fanout_loc->y);

		yes_swap += fabs(new_loc->x - fanout_loc->x);
		yes_swap += fabs(new_loc->y - fanout_loc->y);
	}
	return yes_swap - no_swap;
}
#endif //USE_RISCV_VECTOR
