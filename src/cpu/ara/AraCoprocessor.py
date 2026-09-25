from m5.params import *
from m5.objects.ClockedObject import ClockedObject

class AraCoprocessor(ClockedObject):
    type = 'AraCoprocessor'
    cxx_header = "cpu/ara/ara_coprocessor.hh"
    cxx_class = "gem5::AraCoprocessor"

    num_lanes = Param.Unsigned(4, "Number of vector lanes")
    vlen = Param.Unsigned(4096, "Vector register length in bits")
    datapath_width = Param.Unsigned(64, "Datapath width per lane in bits")
    axi_data_width = Param.Unsigned(256, "AXI data width in bits for the VLSU")
    
    dcache_port = RequestPort("Data port for Vector Load/Store Unit")
