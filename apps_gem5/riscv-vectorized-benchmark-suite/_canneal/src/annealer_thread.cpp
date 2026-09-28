// annealer_thread.cpp
//
// Created by Daniel Schwartz-Narbonne on 14/04/07.
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

#ifdef ENABLE_THREADS
#include <pthread.h>
#endif
 
#include "assert.h"
#include "annealer_thread.h"
#include "location_t.h"
#include "annealer_types.h"
#include "netlist_elem.h"
#include "rng.h"

#include "printf.h"

// RISC-V VECTOR Version by Cristóbal Ramírez Lazo, "Barcelona 2019"
#ifdef USE_RISCV_VECTOR
#include <riscv_vector.h>
#include "common/vector_defines.h"
#endif

//*****************************************************************************************
//
//*****************************************************************************************
void annealer_thread::Run()
{
    int accepted_good_moves=0;
    int accepted_bad_moves=-1;
    double T = _start_temp;
    Rng rng; //store of randomness

    long a_id;
    long b_id;
    
    netlist_elem* a = _netlist->get_random_element(&a_id, NO_MATCHING_ELEMENT, &rng);
    // printf("Initial element name: %s (ID: %ld)\n", a->item_name, a_id);
    netlist_elem* b = _netlist->get_random_element(&b_id, NO_MATCHING_ELEMENT, &rng);
    // printf("Initial element name: %s (ID: %ld)\n", b->item_name, b_id);

    int temp_steps_completed=0;

    while(keep_going(temp_steps_completed, accepted_good_moves, accepted_bad_moves)){
        printf("Temperature: %f step: %d good moves: %d bad moves: %d\n", T, 
                         temp_steps_completed, accepted_good_moves, accepted_bad_moves);
        T = T / 1.5;
        accepted_good_moves = 0;
        accepted_bad_moves = 0;

        for (int i = 0; i < _moves_per_thread_temp; i++) {
            // printf("Swap iter:%d\n", i+1);
            a = b;
            a_id = b_id;
            b = _netlist->get_random_element(&b_id, a_id, &rng);
    #ifdef USE_RISCV_VECTOR
            routing_cost_t delta_cost = calculate_delta_routing_cost_vector(a,b);
    #else // !USE_RISCV_VECTOR
            routing_cost_t delta_cost = calculate_delta_routing_cost(a,b);
    #endif // !USE_RISCV_VECTOR

            // printf("Delta cost: %f\n", delta_cost);

            move_decision_t is_good_move = accept_move(delta_cost, T, &rng);

            //make the move, and update stats:
            if (is_good_move == move_decision_accepted_bad){
                accepted_bad_moves++;
                _netlist->swap_locations(a,b);
            } else if (is_good_move == move_decision_accepted_good){
                accepted_good_moves++;
                _netlist->swap_locations(a,b);
            } else if (is_good_move == move_decision_rejected){
                //no need to do anything for a rejected move
            }
        }

        temp_steps_completed++;
#ifdef ENABLE_THREADS
        pthread_barrier_wait(&_barrier);
#endif
    }
}

//*****************************************************************************************
//
//*****************************************************************************************
annealer_thread::move_decision_t annealer_thread::accept_move(routing_cost_t delta_cost, double T, Rng* rng)
{
    //always accept moves that lower the cost function
    if (delta_cost < 0){
        return move_decision_accepted_good;
    }else {
        double random_value = rng->drand();
        double boltzman = exp(- delta_cost/T);
        if (boltzman > random_value){
            return move_decision_accepted_bad;
        } else {
            return move_decision_rejected;
        }
    }
}

//*****************************************************************************************
//  If get turns out to be expensive, I can reduce the # by passing it into the swap cost fcn
//*****************************************************************************************
#ifdef USE_RISCV_VECTOR
routing_cost_t annealer_thread::calculate_delta_routing_cost_vector(netlist_elem* a, netlist_elem* b/*, __epi_2xi1  xMask2*/)
{
    routing_cost_t delta_cost=0.0;

    int a_fan_size = a->fanin_count + a->fanout_count;
    int b_fan_size = b->fanin_count + b->fanout_count;
    location_t* a_loc = a->present_loc.Get();
    location_t* b_loc = b->present_loc.Get();


    if((a_fan_size > 0) | (b_fan_size > 0))
    {
        int max_vl = (a_fan_size > b_fan_size) ? a_fan_size*2 : b_fan_size*2;
#ifdef INTRINSICS
        unsigned long int gvl   = __riscv_vsetvl_e32m1(max_vl);
        _MMR_MASK_i32  xMask     = _MM_LOAD_MASK_u32((const uint8_t *)&mask[0],gvl);
        _MMR_i32 xAFanin_loc     = _MM_MERGE_i32(_MM_SET_i32(a_loc->y,gvl),_MM_SET_i32(a_loc->x,gvl),xMask,gvl);
        _MMR_i32 xBFanin_loc     = _MM_MERGE_i32(_MM_SET_i32(b_loc->y,gvl),_MM_SET_i32(b_loc->x,gvl),xMask,gvl);
        if(a_fan_size > 0) {
            delta_cost = a->swap_cost_vector(xAFanin_loc,xBFanin_loc,a_fan_size);
        }
        if(b_fan_size > 0) {
            delta_cost = delta_cost + b->swap_cost_vector(xBFanin_loc,xAFanin_loc,b_fan_size);
        }
#else
        unsigned long int gvl;
        asm volatile ("vsetvli %0, %1, e32, m1, ta, ma" : "=r"(gvl) : "r"(max_vl));
        asm volatile ("vlm.v v0, (%0)"::"r"(&mask[0]));
        asm volatile ("vmv.v.x v4, %0"::"r"(a_loc->x));
        asm volatile ("vmv.v.x v8, %0"::"r"(a_loc->y));
        asm volatile ("vmv.v.x v12, %0"::"r"(b_loc->x));
        asm volatile ("vmv.v.x v16, %0"::"r"(b_loc->y));
        asm volatile ("vmerge.vvm v4, v8, v4, v0"); // xAFanin_loc
        asm volatile ("vmerge.vvm v12, v16, v12, v0"); // xBFanin_loc
        if(a_fan_size > 0) {
            delta_cost = a->swap_cost_vector(a_fan_size);
        }
        if(b_fan_size > 0) {
            delta_cost = delta_cost - b->swap_cost_vector(b_fan_size); 
            // (-) here since we don't swap the registers v4 and v12 within the swap_cost_vector function
        }
#endif        

    }

    return delta_cost;
}

#else // !USE_RISCV_VECTOR
//*****************************************************************************************
//  If get turns out to be expensive, I can reduce the # by passing it into the swap cost fcn
//*****************************************************************************************
routing_cost_t annealer_thread::calculate_delta_routing_cost(netlist_elem* a, netlist_elem* b)
{
    routing_cost_t delta_cost=0.0;

    location_t* a_loc = a->present_loc.Get();
    location_t* b_loc = b->present_loc.Get();

    delta_cost = a->swap_cost(a_loc, b_loc);
    delta_cost = delta_cost + b->swap_cost(b_loc, a_loc);

    return delta_cost;
}
#endif // !USE_RISCV_VECTOR
//*****************************************************************************************
//  Check whether design has converged or maximum number of steps has reached
//*****************************************************************************************
bool annealer_thread::keep_going(int temp_steps_completed, int accepted_good_moves, int accepted_bad_moves)
{
    bool rv;

    if(_number_temp_steps == -1) {
        //run until design converges
        rv = _keep_going_global_flag && (accepted_good_moves > accepted_bad_moves);
        if(!rv) _keep_going_global_flag = false; // signal we have converged
    } else {
        //run a fixed amount of steps
        rv = temp_steps_completed < _number_temp_steps;
    }

    return rv;
}
