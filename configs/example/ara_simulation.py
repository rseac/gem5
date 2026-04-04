
# Copyright (c) 2025 Google LLC
# All rights reserved.

import m5
from m5.objects import *
from m5.util import addToPath
import os
import sys

# Add src to path to import AraConfig
sys.path.append(os.path.join(os.path.dirname(os.path.abspath(__file__)), '../../src'))

from cpu.o3.AraConfig import AraO3CPU

# Defines the system
system = System()

# Set the clock frequency of the system
system.clk_domain = SrcClockDomain()
system.clk_domain.clock = '1GHz'
system.clk_domain.voltage_domain = VoltageDomain()

# Set up the memory mode to timing (needed for O3CPU)
system.mem_mode = 'timing'
system.mem_ranges = [AddrRange('512MB')]

# Use the AraO3 CPU (automatically sets up ARA Functional Units)
system.cpu = AraO3CPU()

# Create the interconnect
system.membus = SystemXBar()

# Connect the CPU cache ports to the membus
# (Using simple ports for demonstration, bypassing caches for simplicity
# unless strictly required, but O3 typically needs caches. 
# Let's connect directly to membus for a minimal functional script 
# or use standard caches if imports allow. 
# For minimal latency testing, direct connection is often cleaner to isolate pipeline)
system.cpu.icache_port = system.membus.cpu_side_ports
system.cpu.dcache_port = system.membus.cpu_side_ports

# Connect the interrupt controller
system.cpu.createInterruptController()

# Set up the memory controller
system.mem_ctrl = MemCtrl()
system.mem_ctrl.dram = DDR3_1600_8x8()
system.mem_ctrl.dram.range = system.mem_ranges[0]
system.mem_ctrl.port = system.membus.mem_side_ports

import argparse

# Parse arguments
parser = argparse.ArgumentParser(description='Run gem5 simulation with ARA latencies.')
parser.add_argument('cmd', type=str, help='Path to the binary to execute')
args = parser.parse_args()

# Create the process
process = Process()
process.cmd = [args.cmd]
system.cpu.workload = process
system.cpu.createThreads()

# Set up the system workload (Critical fix for !seWorkload error)
system.workload = SEWorkload.init_compatible(args.cmd)

# Instantiate the system
root = Root(full_system=False, system=system)
m5.instantiate()

print(f"Beginning simulation with ARA Latencies configuration!")
# Access fuPool from the first IQ (as set up in AraO3CPU constructor)
if hasattr(system.cpu, 'instQueues') and len(system.cpu.instQueues) > 0:
    pool_name = type(system.cpu.instQueues[0].fuPool).__name__
    print(f"CPU IQ[0] FUPool Class: {pool_name}")
else:
    print(f"CPU Type: {type(system.cpu).__name__}")
exit_event = m5.simulate()

print('Exiting @ tick {} because {}'.format(
      m5.curTick(), exit_event.getCause()))

# Output cycles (assuming 1GHz clock as configured above)
# 1GHz = 1000 ps period (default gem5 tick is 1ps)
cycles = m5.curTick() / 1000
print(f"Total Execution Cycles: {int(cycles)}")

