// netlist.h
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


#ifndef NETLIST_H
#define NETLIST_H

#include "annealer_types.h"
#include "netlist_elem.h"
#include "location_t.h"
#include "rng.h"

#include "printf.h"

// For compiled netlist data, include the generated sizes
#include "netlist_sizes.h"

const long NO_MATCHING_ELEMENT = -1;

class netlist_elem;
class location_t;
class Rng;

class netlist
{
public:
	netlist(bool use_compiled_data); //ctor with compiled static data
	void get_random_pair(netlist_elem** a, netlist_elem** b, Rng* rng); // will return an element that we have a valid mutex on
	void swap_locations(netlist_elem* elem_a, netlist_elem* elem_b);
	void shuffle(Rng* rng);
	netlist_elem* netlist_elem_from_loc(location_t& loc);
	netlist_elem* netlist_elem_from_name(const char* name);
	routing_cost_t total_routing_cost();
	void print_locations(const char* filename);
	void release(netlist_elem* elem);
	netlist_elem* get_random_element(long* elem_id, long different_from, Rng* rng);
	
protected:
	unsigned _num_elements;
	unsigned _max_x;
	unsigned _max_y;
	unsigned _chip_size;
	unsigned _total_used;
	netlist_elem _elements[NETLIST_MAX_CHIP_SIZE]; // C-style fixed-size array
	location_t _locations[NETLIST_MAX_X][NETLIST_MAX_Y]; // C-style 2D fixed-size array
	netlist_elem* find_elem_by_name(const char* name); // Linear search for element by name
	//due to the pointers, perhaps I should make the copy operator protected to prevent copying
};



#endif

