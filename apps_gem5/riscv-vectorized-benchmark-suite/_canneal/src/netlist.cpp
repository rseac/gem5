// netlist.cpp
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


#include "location_t.h"
#include "netlist.h"
#include "netlist_elem.h"
#include "rng.h"

# include <string.h>
#include <assert.h>

#include "printf.h"

// External symbols from compiled assembly netlist data (index-based)
extern const unsigned long compiled_num_elements[];
extern const unsigned long compiled_max_x[];
extern const unsigned long compiled_max_y[];
extern const unsigned long compiled_total_used[];
extern const char*         compiled_element_names[];
extern const unsigned long compiled_fanin_flat[];
extern const unsigned long compiled_fanin_offsets[];
extern const unsigned long compiled_fanin_counts[];
extern const unsigned long compiled_fanout_flat[];
extern const unsigned long compiled_fanout_offsets[];
extern const unsigned long compiled_fanout_counts[];
extern const unsigned long compiled_fanlocs_flat[];
extern const unsigned long compiled_fanlocs_offsets[];
extern const unsigned long compiled_fanlocs_counts[];

using namespace std;


void netlist::release(netlist_elem* elem)
{
	return;
}


//*****************************************************************************************
//not thread safe, tho i could make it so if i needed to
//only look at the non_blank elements:  this saves some time
//*****************************************************************************************
routing_cost_t netlist::total_routing_cost()
{
	routing_cost_t rval = 0;
	for (unsigned i = 0; i < _num_elements; i++) {
		netlist_elem* elem = &_elements[i];
		if (elem->present_loc.Get() != NULL) {
			rval += elem->routing_cost_given_loc(*(elem->present_loc.Get()));
		}
	}
	return rval / 2; //since routing_cost calculates both input and output routing, we have double counted
}



//*****************************************************************************************
// just use a simple shuffle algorithm
//*****************************************************************************************
void netlist::shuffle(Rng* rng)
{
	for (int i = 0; i < _max_x * _max_y * 1000; i++){
		netlist_elem *a, *b;
		get_random_pair(&a, &b, rng);
		swap_locations(a, b);
	}
}


//*****************************************************************************************
//  SYNC need chris' atomic swap algorithm
//*****************************************************************************************
void netlist::swap_locations(netlist_elem* elem_a, netlist_elem* elem_b)
{
	//and swap the locations stored in the actual netlist_elem
	elem_a->present_loc.Swap(elem_b->present_loc);
}


//*****************************************************************************************
//returns an elemment that is different from the different_from element
// if different_from == NO_MATCHING_ELEMENT then returns any element
//*****************************************************************************************
netlist_elem* netlist::get_random_element(long* elem_id, long different_from, Rng* rng)
{
	// printf("Getting random element different from ID: %ld\n", different_from);
	long id = rng->rand(_chip_size);
	// printf("Random element ID: %ld\n", id);
	netlist_elem* elem = &(_elements[id]);
	
	//loop until we get a non duplicate element
	//-1 is not a possible element, will never enter this loop in that case
	//if it doesn't work, try a new one
	while (id == different_from){ 
		id = rng->rand(_chip_size);
		// printf("Random element ID: %ld\n", id);
		elem = &(_elements[id]);
	}
	*elem_id=id;
	return elem;
}


//*****************************************************************************************
//assumption: will return elements a, b which we can get a valid lock on
//*****************************************************************************************
void netlist::get_random_pair(netlist_elem** a, netlist_elem** b, Rng* rng)
{
	//get a random element
	long id_a = rng->rand(_chip_size);
	netlist_elem* elem_a = &(_elements[id_a]);
	
	//now do the same for b
	long id_b = rng->rand(_chip_size);
	netlist_elem* elem_b = &(_elements[id_b]);

	//keep trying new elements until we get one that works
	//get required locks automatically rolls back if it fails
	//keep going until we get
	while (id_b == id_a){ //no duplicate elements
		//if it doesn't work, try a new one
		id_b = rng->rand(_chip_size);
		elem_b = &(_elements[id_b]);
	}

	*a = elem_a;
	*b = elem_b;
	return;
}

//*****************************************************************************************
//  No longer easy to implement
//*****************************************************************************************
netlist_elem* netlist::netlist_elem_from_loc(location_t& loc)
{
	assert(false);
	return NULL;
}

//*****************************************************************************************
//
//*****************************************************************************************
netlist_elem* netlist::netlist_elem_from_name(const char* name)
{
	return find_elem_by_name(name);
}

//*****************************************************************************************
// Linear search for element by name
//*****************************************************************************************
netlist_elem* netlist::find_elem_by_name(const char* name)
{
	for (unsigned i = 0; i < _total_used; i++) {
		if (strcmp(_elements[i].item_name, name) == 0) {
			return &_elements[i];
		}
	}
	return NULL;
}

//*****************************************************************************************
// Constructor using pre-computed index-based netlist data from gen_data.py.
// All name-to-index resolution and fanin/fanout computation was done at generation time.
// This constructor only sets up the location grid and copies pre-computed index-based
// connections into pointer arrays — no string lookups needed.
//*****************************************************************************************
netlist::netlist(bool use_compiled_data)
{
	if (!use_compiled_data) {
		assert(false);
	}

// #ifdef USE_COMPILED_NETLIST
	_num_elements = compiled_num_elements[0];
	_max_x        = compiled_max_x[0];
	_max_y        = compiled_max_y[0];
	_chip_size    = _max_x * _max_y;
	_total_used   = compiled_total_used[0];
	assert(_num_elements < _chip_size);

	// 1. Initialize location grid + element defaults
	printf("Initializing netlist with %lu elements, max_x=%lu, max_y=%lu\n",
	       _num_elements, _max_x, _max_y);
	unsigned i_elem = 0;
	for (int x = 0; x < (int)_max_x; x++) {
		for (int y = 0; y < (int)_max_y; y++) {
			location_t* loc = &_locations[x][y];
			loc->x = x;
			loc->y = y;
			_elements[i_elem].present_loc.Set(loc);
			_elements[i_elem].fanin_count    = 0;
			_elements[i_elem].fanout_count   = 0;
			_elements[i_elem].fan_locs_count = 0;
			strncpy(_elements[i_elem].item_name, "empty", MAX_ELEMENT_NAME_LENGTH - 1);
			_elements[i_elem].item_name[MAX_ELEMENT_NAME_LENGTH - 1] = '\0';
			i_elem++;
		}
	}

	// 2. Copy element names (direct index, no search)
	printf("Copying element names...\n");
	for (unsigned i = 0; i < _total_used; i++) {
		strncpy(_elements[i].item_name, compiled_element_names[i],
		        MAX_ELEMENT_NAME_LENGTH - 1);
		_elements[i].item_name[MAX_ELEMENT_NAME_LENGTH - 1] = '\0';
	}

	// 3. Fanin pointers from pre-computed indices
	printf("Setting up fanin pointers...\n");
	for (unsigned i = 0; i < _total_used; i++) {
		unsigned off = compiled_fanin_offsets[i];
		unsigned cnt = compiled_fanin_counts[i];
		_elements[i].fanin_count = cnt;
		for (unsigned j = 0; j < cnt; j++) {
			_elements[i].fanin[j] = &_elements[compiled_fanin_flat[off + j]];
		}
	}

	// 4. Fanout pointers from pre-computed indices
	printf("Setting up fanout pointers...\n");
	for (unsigned i = 0; i < _total_used; i++) {
		unsigned off = compiled_fanout_offsets[i];
		unsigned cnt = compiled_fanout_counts[i];
		_elements[i].fanout_count = cnt;
		for (unsigned j = 0; j < cnt; j++) {
			_elements[i].fanout[j] = &_elements[compiled_fanout_flat[off + j]];
		}
	}

#ifdef USE_RISCV_VECTOR
	// 5. Fan_locs pointers from pre-computed indices
	printf("Setting up fan_locs pointers...\n");
	for (unsigned i = 0; i < _total_used; i++) {
		unsigned off = compiled_fanlocs_offsets[i];
		unsigned cnt = compiled_fanlocs_counts[i];
		_elements[i].fan_locs_count = cnt;
		for (unsigned j = 0; j < cnt; j++) {
			_elements[i].fan_locs[j] =
				(unsigned long *)&_elements[compiled_fanlocs_flat[off + j]].present_loc;
		}
	}
#endif

// #else
// 	assert(false); // USE_COMPILED_NETLIST must be defined
// #endif
}
