// netlist_elem.h
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


#ifndef NETLIST_ELEM_H
#define NETLIST_ELEM_H

#include "AtomicPtr.h"
#include "location_t.h"
#include "annealer_types.h"

#ifdef USE_RISCV_VECTOR
#include "common/vector_defines.h"
#endif

// Maximum fanin/fanout connections per element (C-style array size)
#define MAX_FAN_LOCS_PER_ELEM 512
#define MAX_FANIN_PER_ELEM 2048
#define MAX_FANOUT_PER_ELEM 2048
#define MAX_ELEMENT_NAME_LENGTH 256

using threads::AtomicPtr;

class netlist_elem{
public:
	netlist_elem();
	routing_cost_t routing_cost_given_loc(location_t loc);
#ifdef USE_RISCV_VECTOR
#ifdef INTRINSICS
	routing_cost_t swap_cost_vector(_MMR_i32 xAFanin_loc ,_MMR_i32 xBFanin_loc ,int a_fan_size);
#else
	routing_cost_t swap_cost_vector(int a_fan_size);
#endif
#else // !USE_RISCV_VECTOR
	routing_cost_t swap_cost(location_t* old_loc, location_t* new_loc);
#endif //USE_RISCV_VECTOR

public:
	char item_name[MAX_ELEMENT_NAME_LENGTH]; //C-style fixed-size string for the name
	AtomicPtr<location_t> present_loc;
	//C-style fixed-size arrays for fanin/fanout (replace std::vector)
	netlist_elem* fanin[MAX_FANIN_PER_ELEM];
	unsigned int fanin_count; // Number of entries in fanin array
	netlist_elem* fanout[MAX_FANOUT_PER_ELEM];
	unsigned int fanout_count; // Number of entries in fanout array
	//C-style fixed-size array for fanin/fanout locations (RISCV vector operations)
	unsigned long * fan_locs[MAX_FAN_LOCS_PER_ELEM];
	unsigned int fan_locs_count; // Number of entries in fan_locs array
protected:
};


#endif
