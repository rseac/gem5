import os
import sys
import m5
from m5.objects import *

if len(sys.argv) < 2:
    print("Usage: run_gem5_ara.py <benchmark.elf> [args...]")
    sys.exit(1)

binary = sys.argv[1]
binary_args = sys.argv[2:]

system = System()
system.clk_domain = SrcClockDomain()
system.clk_domain.clock = "1GHz"
system.clk_domain.voltage_domain = VoltageDomain()

system.mem_mode = "timing"
system.mem_ranges = [AddrRange(start=0x80000000, size="2GB")]
system.cpu = MinorCPU()

# RiscvISA's own vlen (default 256 bits) is the *real* architectural VLEN
# that vsetvli honors - it is a separate parameter from AraCoprocessor's
# own vlen below (which only sizes the coprocessor's internal mock model)
# and was previously left unset, silently running every benchmark at 1/16
# of the RTL baseline's VLEN=4096 and thus ~16x more vector-loop trips.
system.cpu.isa = [RiscvISA(vlen=4096, elen=64)]

system.cpu.araCoprocessor = AraCoprocessor(num_lanes=4, vlen=4096)
system.membus = SystemXBar(width=512, forward_latency=0, response_latency=0, snoop_response_latency=0, header_latency=0)
system.cpu.icache_port = system.membus.cpu_side_ports
system.cpu.dcache_port = system.membus.cpu_side_ports
system.cpu.araCoprocessor.dcache_port = system.membus.cpu_side_ports
system.cpu.createInterruptController()

system.mem_ctrl = SimpleMemory(latency="0ns", bandwidth="64GB/s")
system.mem_ctrl.range = system.mem_ranges[0]
system.mem_ctrl.port = system.membus.mem_side_ports
system.system_port = system.membus.cpu_side_ports

system.workload = SEWorkload.init_compatible(binary)
process = Process()
process.cmd = [binary] + binary_args
system.cpu.workload = process
system.cpu.createThreads()

root = Root(full_system=False, system=system)
m5.instantiate()

# A normal glibc-linked SE-mode binary gets its stack/heap/BSS mapped
# automatically by Process/SEWorkload - no manual identity-mapping of
# bare-metal-testbench-style fixed addresses is needed here.

print("Beginning simulation for %s!" % binary)
exit_event = m5.simulate()
print("Exiting @ tick %i because %s" % (m5.curTick(), exit_event.getCause()))
