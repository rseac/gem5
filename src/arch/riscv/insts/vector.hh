/*
 * Copyright (c) 2022 PLCT Lab
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are
 * met: redistributions of source code must retain the above copyright
 * notice, this list of conditions and the following disclaimer;
 * redistributions in binary form must reproduce the above copyright
 * notice, this list of conditions and the following disclaimer in the
 * documentation and/or other materials provided with the distribution;
 * neither the name of the copyright holders nor the names of its
 * contributors may be used to endorse or promote products derived from
 * this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 * OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#ifndef __ARCH_RISCV_INSTS_VECTOR_HH__
#define __ARCH_RISCV_INSTS_VECTOR_HH__

#include <string>
#include <optional>

#include "arch/riscv/faults.hh"
#include "arch/riscv/insts/static_inst.hh"
#include "arch/riscv/isa.hh"
#include "arch/riscv/pcstate.hh"
#include "arch/riscv/regs/misc.hh"
#include "arch/riscv/utility.hh"
#include "cpu/exec_context.hh"
#include "cpu/static_inst.hh"
#include "cpu/base.hh"
#include "cpu/latency_model.hh"
#include "debug/VectorTiming.hh"

namespace gem5
{

namespace RiscvISA
{

float getVflmul(uint32_t vlmul_encoding);

inline uint32_t
getSew(uint32_t vsew)
{
    assert(vsew <= 3);
    return (8 << vsew);
}

uint32_t getVlmax(VTYPE vtype, uint32_t vlen);

inline uint32_t
get_emul(uint32_t eew, uint32_t sew, float vflmul, bool is_mask_ldst)
{
    eew = is_mask_ldst ? 1 : eew;
    float vemul = is_mask_ldst ? 1 : (float)eew / sew * vflmul;
    uint32_t emul = vemul < 1 ? 1 : vemul;
    return emul;
}

/**
 * Base class for Vector Config operations
 */
class VConfOp : public RiscvStaticInst
{
  protected:
    uint64_t bit30;
    uint64_t bit31;
    uint64_t zimm10;
    uint64_t zimm11;
    uint64_t uimm;
    uint32_t elen;
    uint32_t vlen;
    VConfOp(const char *mnem, ExtMachInst _extMachInst, uint32_t _elen,
            uint32_t _vlen, OpClass __opClass)
        : RiscvStaticInst(mnem, _extMachInst, __opClass),
          bit30(_extMachInst.bit30),
          bit31(_extMachInst.bit31),
          zimm10(_extMachInst.zimm_vsetivli),
          zimm11(_extMachInst.zimm_vsetvli),
          uimm(_extMachInst.uimm_vsetivli),
          elen(_elen),
          vlen(_vlen)
    {
        this->flags[IsVector] = true;
    }

    std::string
    generateDisassembly(Addr pc,
                        const loader::SymbolTable *symtab) const override;

    std::string generateZimmDisassembly() const;
};

inline uint8_t
checked_vtype(bool vill, uint8_t vtype)
{
    panic_if(vill, "vill has been set");
    const uint8_t vsew = bits(vtype, 5, 3);
    panic_if(vsew >= 0b100, "vsew: %#x not supported", vsew);
    const uint8_t vlmul = bits(vtype, 2, 0);
    panic_if(vlmul == 0b100, "vlmul: %#x not supported", vlmul);
    return vtype;
}

class VectorNonSplitInst : public RiscvStaticInst
{
  protected:
    uint32_t vl;
    uint8_t vtype;
    uint32_t elen;
    uint32_t vlen;
    VectorNonSplitInst(const char *mnem, ExtMachInst _machInst,
                       OpClass __opClass, uint32_t _elen, uint32_t _vlen)
        : RiscvStaticInst(mnem, _machInst, __opClass),
          vl(_machInst.vl),
          vtype(_machInst.vtype8),
          elen(_elen),
          vlen(_vlen)
    {
        this->flags[IsVector] = true;
    }

    std::string
    generateDisassembly(Addr pc,
                        const loader::SymbolTable *symtab) const override;
};

class VectorMacroInst : public RiscvMacroInst
{
  protected:
    uint32_t vl;
    uint8_t vtype;
    uint32_t elen;
    uint32_t vlen;
    int oldDstIdx = -1;
    int vmsrcIdx = -1;
    const uint8_t vsew;
    const int8_t vlmul;
    const uint32_t sew;
    const float vflmul;

    VectorMacroInst(const char *mnem, ExtMachInst _machInst, OpClass __opClass,
                    uint32_t _elen, uint32_t _vlen)
        : RiscvMacroInst(mnem, _machInst, __opClass),
          vl(_machInst.vl),
          vtype(_machInst.vtype8),
          elen(_elen),
          vlen(_vlen),
          vsew(_machInst.vtype8.vsew),
          vlmul(vtype_vlmul(_machInst.vtype8)),
          sew((8 << vsew)),
          vflmul(vlmul < 0 ? (1.0 / (1 << (-vlmul))) : (1 << vlmul))
    {
        this->flags[IsVector] = true;
    }
};

class VectorMicroInst : public RiscvMicroInst
{
  protected:
    uint32_t microVl;
    uint32_t microIdx;
    uint8_t vtype;
    uint32_t elen;
    uint32_t vlen;
    int oldDstIdx = -1;
    int vmsrcIdx = -1;
    const uint8_t vsew;
    const int8_t vlmul;
    const uint32_t sew;
    const float vflmul;

    VectorMicroInst(const char *mnem, ExtMachInst _machInst, OpClass __opClass,
                    uint32_t _microVl, uint32_t _microIdx, uint32_t _elen,
                    uint32_t _vlen)
        : RiscvMicroInst(mnem, _machInst, __opClass),
          microVl(_microVl),
          microIdx(_microIdx),
          vtype(_machInst.vtype8),
          elen(_elen),
          vlen(_vlen),
          vsew(_machInst.vtype8.vsew),
          vlmul(vtype_vlmul(_machInst.vtype8)),
          sew((8 << vsew)),
          vflmul(vlmul < 0 ? (1.0 / (1 << (-vlmul))) : (1 << vlmul))
    {
        this->flags[IsVector] = true;
    }

  private:
    inline int opOccupancy(gem5::BaseCPU *cpu) const 
    {
        unsigned NrLanes = cpu->vectorTimingThroughput;
        panic_if(NrLanes == 0,
                 "Vector timing throughput must be > 0");
        panic_if(sew == 0 || sew > 64,
                 "Unsupported SEW %u for ARA", sew);
        // Ara sends ELEN (64) bits to alu per lane * cycle regardless of scalar element width (sew).
        // The vector instruction occupies the unit just for the act of sending itself (ignoring pipeline latency)
        unsigned bitsPerCycle = 64 * NrLanes;
        unsigned microVlBits = microVl * sew;
        // Equivalent to exact division with ceil(microVlBits / bitsPerCycle).
        return std::max(1U, (microVlBits + bitsPerCycle - 1) / bitsPerCycle);
    }

  protected:
    std::optional<Cycles>
    dynamicIssueLatency(ThreadContext *tc) const override
    {
        switch (opClass()) {
          case enums::SimdAdd:
          case enums::SimdAddAcc:
          case enums::SimdAlu:
          case enums::SimdCmp:
          case enums::SimdCvt:
          case enums::SimdMisc:
          case enums::SimdShift:
          case enums::SimdShiftAcc:
          case enums::SimdConfig:
          case enums::SimdMult:
          case enums::SimdMultAcc:
          case enums::SimdMatMultAcc:
          case enums::SimdFloatAdd:
          case enums::SimdFloatAlu:
          case enums::SimdFloatMult:
          case enums::SimdFloatMultAcc:
          case enums::SimdFloatMatMultAcc:
          case enums::SimdFloatCmp:
          case enums::SimdFloatMisc:
          case enums::SimdFloatCvt:
          case enums::SimdUnitStrideLoad:
          case enums::SimdUnitStrideStore:
          case enums::SimdUnitStrideMaskLoad:
          case enums::SimdUnitStrideMaskStore:
          case enums::SimdUnitStrideSegmentedLoad:
          case enums::SimdUnitStrideSegmentedStore:
          case enums::SimdWholeRegisterLoad:
          case enums::SimdWholeRegisterStore:
          case enums::SimdStridedLoad:
          case enums::SimdStridedStore:
          case enums::SimdIndexedLoad:
          case enums::SimdIndexedStore:
          case enums::SimdExt:
          case enums::SimdFloatExt:
          case enums::SimdReduceAdd:
          case enums::SimdReduceAlu:
          case enums::SimdReduceCmp:
          case enums::SimdFloatReduceAdd:
          case enums::SimdFloatReduceCmp:
            break;
          default:
            return std::nullopt;
        }

        auto cpu = tc->getCpuPtr();
        return Cycles(opOccupancy(cpu));
    }

    Cycles
    dynamicOpLatency(ThreadContext *tc) const override
    {
        auto cpu = tc->getCpuPtr();
        int occupancy = opOccupancy(cpu);

        if (cpu->latencyModel) {
            Cycles pipe_depth = cpu->latencyModel->getLatency(opClass(), vsew, microVl);
            int floor = cpu->latencyModel->getDispatchFloor();
            
            // Special Case: Iterative units (Div/Sqrt) are serial.
            // The occupancy is (cycles_per_element * microVl).
            // We handle this by returning the full iterative time from getLatency
            // and using 0 for the "pipelined" occupancy here.
            int unit_occupancy = occupancy;
            if (opClass() == enums::SimdDiv || opClass() == enums::SimdFloatDiv || 
                opClass() == enums::SimdFloatSqrt) {
                unit_occupancy = 0;
            }

            int total = (int)pipe_depth + unit_occupancy;
            if (total < floor) total = floor;
            
            return Cycles(total);
        }

        // Legacy Fallback: Hardcoded reconciled ARA values
        // Total execution latency = pipeline depth + occupancy
        int pipeline_lat = 0;
        switch (opClass()) {
          case enums::SimdAdd:
          case enums::SimdAlu:
          case enums::SimdShift:
          case enums::SimdMisc:
          case enums::SimdCmp:
            pipeline_lat = 6;
            break;
          case enums::SimdFloatAdd:
          case enums::SimdFloatAlu:
          case enums::SimdFloatMult:
          case enums::SimdFloatMultAcc:
          case enums::SimdFloatMatMultAcc:
            pipeline_lat = vsew + 8;
            break;
          case enums::SimdFloatCvt:
            pipeline_lat = 5;
            break;
          case enums::SimdFloatDiv:
          case enums::SimdFloatSqrt:
            pipeline_lat = 19;
            break;
          case enums::SimdDiv:
            pipeline_lat = (8 << vsew) + 9;
            break;
          case enums::SimdMult:
          case enums::SimdMultAcc:
            pipeline_lat = ((sew == 8) ? 0 : 1) + 7;
            break;
          case enums::SimdUnitStrideLoad:
            pipeline_lat = 23;
            break;
          case enums::SimdUnitStrideStore:
            pipeline_lat = 19;
            break;
          default:
            pipeline_lat = 6;
            break;
        }

        return Cycles(pipeline_lat + occupancy);
    }

    Cycles
    chainingLatency(ThreadContext *tc) const override
    {
        auto cpu = tc->getCpuPtr();
        if (cpu->latencyModel) {
            // For chaining, we only care about when the FIRST element is ready.
            // This is the functional unit pipeline depth + 1 cycle for issue.
            Cycles pipe_depth = cpu->latencyModel->getLatency(opClass(), vsew, microVl);
            int floor = cpu->latencyModel->getDispatchFloor();
            
            // Readiness = max(Pipe + 1, Floor)
            int readiness = (int)pipe_depth + 1;
            if (readiness < floor) readiness = floor;
            
            return Cycles(readiness);
        }

        // Fallback for legacy hardcoded model
        return dynamicOpLatency(tc);
    }
};

class VectorNopMicroInst : public RiscvMicroInst
{
  protected:
    const Fault fault;

  public:
    VectorNopMicroInst(ExtMachInst _machInst, const Fault &fault = NoFault)
        : RiscvMicroInst("vnop", _machInst, No_OpClass), fault(fault)
    {}

    Fault
    execute(ExecContext *xc, trace::InstRecord *traceData) const override
    {
        return fault;
    }

    std::string
    generateDisassembly(Addr pc,
                        const loader::SymbolTable *symtab) const override
    {
        std::stringstream ss;
        ss << mnemonic;
        return ss.str();
    }
};

class VectorArithMicroInst : public VectorMicroInst
{
  protected:
    VectorArithMicroInst(const char *mnem, ExtMachInst _machInst,
                         OpClass __opClass, uint32_t _microVl,
                         uint32_t _microIdx, uint32_t _elen, uint32_t _vlen)
        : VectorMicroInst(mnem, _machInst, __opClass, _microVl, _microIdx,
                          _elen, _vlen)
    {}

    std::string
    generateDisassembly(Addr pc,
                        const loader::SymbolTable *symtab) const override;
};

class VectorArithMacroInst : public VectorMacroInst
{
  protected:
    VectorArithMacroInst(const char *mnem, ExtMachInst _machInst,
                         OpClass __opClass, uint32_t _elen, uint32_t _vlen)
        : VectorMacroInst(mnem, _machInst, __opClass, _elen, _vlen)
    {
        this->flags[IsVector] = true;
    }
    std::string
    generateDisassembly(Addr pc,
                        const loader::SymbolTable *symtab) const override;
};

class VectorVMUNARY0MicroInst : public VectorMicroInst
{
  protected:
    VectorVMUNARY0MicroInst(const char *mnem, ExtMachInst _machInst,
                            OpClass __opClass, uint32_t _microVl,
                            uint32_t _microIdx, uint32_t _elen, uint32_t _vlen)
        : VectorMicroInst(mnem, _machInst, __opClass, _microVl, _microIdx,
                          _elen, _vlen)
    {}

    std::string
    generateDisassembly(Addr pc,
                        const loader::SymbolTable *symtab) const override;
};

class VectorVMUNARY0MacroInst : public VectorMacroInst
{
  protected:
    VectorVMUNARY0MacroInst(const char *mnem, ExtMachInst _machInst,
                            OpClass __opClass, uint32_t _elen, uint32_t _vlen)
        : VectorMacroInst(mnem, _machInst, __opClass, _elen, _vlen)
    {
        this->flags[IsVector] = true;
    }

    std::string
    generateDisassembly(Addr pc,
                        const loader::SymbolTable *symtab) const override;
};

class VectorSlideMacroInst : public VectorMacroInst
{
  protected:
    VectorSlideMacroInst(const char *mnem, ExtMachInst _machInst,
                         OpClass __opClass, uint32_t _elen, uint32_t _vlen)
        : VectorMacroInst(mnem, _machInst, __opClass, _elen, _vlen)
    {
        this->flags[IsVector] = true;
    }

    std::string
    generateDisassembly(Addr pc,
                        const loader::SymbolTable *symtab) const override;
};

class VectorSlideMicroInst : public VectorMicroInst
{
  protected:
    uint32_t vdIdx;
    uint32_t vs2Idx;
    uint32_t vs3Idx;
    VectorSlideMicroInst(const char *mnem, ExtMachInst _machInst,
                         OpClass __opClass, uint32_t _microVl,
                         uint32_t _microIdx, uint32_t _vdIdx, uint32_t _vs2Idx,
                         uint32_t _vs3Idx, uint32_t _elen, uint32_t _vlen)
        : VectorMicroInst(mnem, _machInst, __opClass, _microVl, _microIdx,
                          _elen, _vlen),
          vdIdx(_vdIdx),
          vs2Idx(_vs2Idx),
          vs3Idx(_vs3Idx)
    {}

    std::string
    generateDisassembly(Addr pc,
                        const loader::SymbolTable *symtab) const override;
};

class VectorMemMicroInst : public VectorMicroInst
{
  protected:
    uint32_t offset; // Used to calculate EA.
    Request::Flags memAccessFlags;
    const uint8_t veew;
    const uint32_t eew;

    VectorMemMicroInst(const char *mnem, ExtMachInst _machInst,
                       OpClass __opClass, uint32_t _microVl,
                       uint32_t _microIdx, uint32_t _offset, uint32_t _elen,
                       uint32_t _vlen)
        : VectorMicroInst(mnem, _machInst, __opClass, _microVl, _microIdx,
                          _elen, _vlen),
          offset(_offset),
          memAccessFlags(0),
          veew(_machInst.width),
          eew(width_EEW(veew))
    {}
};

class VectorMemMacroInst : public VectorMacroInst
{
  protected:
    const uint8_t veew;
    const uint32_t eew;
    VectorMemMacroInst(const char *mnem, ExtMachInst _machInst,
                       OpClass __opClass, uint32_t _elen, uint32_t _vlen)
        : VectorMacroInst(mnem, _machInst, __opClass, _elen, _vlen),
          veew(_machInst.width),
          eew(width_EEW(veew))
    {}
};

class VleMacroInst : public VectorMemMacroInst
{
  protected:
    VleMacroInst(const char *mnem, ExtMachInst _machInst, OpClass __opClass,
                 uint32_t _elen, uint32_t _vlen)
        : VectorMemMacroInst(mnem, _machInst, __opClass, _elen, _vlen)
    {}

    std::string
    generateDisassembly(Addr pc,
                        const loader::SymbolTable *symtab) const override;
};

class VseMacroInst : public VectorMemMacroInst
{
  protected:
    VseMacroInst(const char *mnem, ExtMachInst _machInst, OpClass __opClass,
                 uint32_t _elen, uint32_t _vlen)
        : VectorMemMacroInst(mnem, _machInst, __opClass, _elen, _vlen)
    {}

    std::string
    generateDisassembly(Addr pc,
                        const loader::SymbolTable *symtab) const override;
};

class VleMicroInst : public VectorMicroInst
{
  public:
    mutable bool trimVl;
    mutable uint32_t faultIdx;

  protected:
    Request::Flags memAccessFlags;

    VleMicroInst(const char *mnem, ExtMachInst _machInst, OpClass __opClass,
                 uint32_t _microVl, uint32_t _microIdx, uint32_t _elen,
                 uint32_t _vlen)
        : VectorMicroInst(mnem, _machInst, __opClass, _microVl, _microIdx,
                          _elen, _vlen),
          trimVl(false),
          faultIdx(_microVl)
    {
        this->flags[IsLoad] = true;
    }

    std::string
    generateDisassembly(Addr pc,
                        const loader::SymbolTable *symtab) const override;
};

class VseMicroInst : public VectorMicroInst
{
  protected:
    Request::Flags memAccessFlags;

    VseMicroInst(const char *mnem, ExtMachInst _machInst, OpClass __opClass,
                 uint32_t _microVl, uint32_t _microIdx, uint32_t _elen,
                 uint32_t _vlen)
        : VectorMicroInst(mnem, _machInst, __opClass, _microVl, _microIdx,
                          _elen, _vlen)
    {
        this->flags[IsStore] = true;
    }

    std::string
    generateDisassembly(Addr pc,
                        const loader::SymbolTable *symtab) const override;
};

class VlWholeMacroInst : public VectorMemMacroInst
{
  protected:
    VlWholeMacroInst(const char *mnem, ExtMachInst _machInst,
                     OpClass __opClass, uint32_t _elen, uint32_t _vlen)
        : VectorMemMacroInst(mnem, _machInst, __opClass, _elen, _vlen)
    {}

    std::string
    generateDisassembly(Addr pc,
                        const loader::SymbolTable *symtab) const override;
};

class VlWholeMicroInst : public VectorMicroInst
{
  protected:
    Request::Flags memAccessFlags;

    VlWholeMicroInst(const char *mnem, ExtMachInst _machInst,
                     OpClass __opClass, uint32_t _microVl, uint32_t _microIdx,
                     uint32_t _elen, uint32_t _vlen)
        : VectorMicroInst(mnem, _machInst, __opClass, _microVl, _microIdx,
                          _elen, _vlen)
    {}

    std::string
    generateDisassembly(Addr pc,
                        const loader::SymbolTable *symtab) const override;
};

class VsWholeMacroInst : public VectorMemMacroInst
{
  protected:
    VsWholeMacroInst(const char *mnem, ExtMachInst _machInst,
                     OpClass __opClass, uint32_t _elen, uint32_t _vlen)
        : VectorMemMacroInst(mnem, _machInst, __opClass, _elen, _vlen)
    {}

    std::string
    generateDisassembly(Addr pc,
                        const loader::SymbolTable *symtab) const override;
};

class VsWholeMicroInst : public VectorMicroInst
{
  protected:
    Request::Flags memAccessFlags;

    VsWholeMicroInst(const char *mnem, ExtMachInst _machInst,
                     OpClass __opClass, uint32_t _microVl, uint32_t _microIdx,
                     uint32_t _elen, uint32_t _vlen)
        : VectorMicroInst(mnem, _machInst, __opClass, _microVl, _microIdx,
                          _elen, _vlen)
    {}

    std::string
    generateDisassembly(Addr pc,
                        const loader::SymbolTable *symtab) const override;
};

class VlElementMacroInst : public VectorMemMacroInst
{
  protected:
    const bool has_rs2;
    VlElementMacroInst(const char *mnem, ExtMachInst _machInst,
                       OpClass __opClass, bool _has_rs2, uint32_t _elen,
                       uint32_t _vlen)
        : VectorMemMacroInst(mnem, _machInst, __opClass, _elen, _vlen),
          has_rs2(_has_rs2)
    {}

    std::string
    generateDisassembly(Addr pc,
                        const loader::SymbolTable *symtab) const override;
};

class VlElementMicroInst : public VectorMemMicroInst
{
  protected:
    uint32_t regIdx;
    const bool has_rs2;
    VlElementMicroInst(const char *mnem, ExtMachInst _machInst,
                       OpClass __opClass, uint32_t _regIdx, uint32_t _microIdx,
                       uint32_t _microVl, uint32_t _offset, bool _has_rs2,
                       uint32_t _elen, uint32_t _vlen)
        : VectorMemMicroInst(mnem, _machInst, __opClass, _microVl, _microIdx,
                             _offset, _elen, _vlen),
          regIdx(_regIdx),
          has_rs2(_has_rs2)
    {}

    std::string
    generateDisassembly(Addr pc,
                        const loader::SymbolTable *symtab) const override;
};

class VsElementMacroInst : public VectorMemMacroInst
{
  protected:
    const bool has_rs2;
    VsElementMacroInst(const char *mnem, ExtMachInst _machInst,
                       OpClass __opClass, bool _has_rs2, uint32_t _elen,
                       uint32_t _vlen)
        : VectorMemMacroInst(mnem, _machInst, __opClass, _elen, _vlen),
          has_rs2(_has_rs2)
    {}

    std::string
    generateDisassembly(Addr pc,
                        const loader::SymbolTable *symtab) const override;
};

class VsElementMicroInst : public VectorMemMicroInst
{
  protected:
    uint32_t regIdx;
    const bool has_rs2;
    VsElementMicroInst(const char *mnem, ExtMachInst _machInst,
                       OpClass __opClass, uint32_t _regIdx, uint32_t _microIdx,
                       uint32_t _microVl, uint32_t _offset, bool _has_rs2,
                       uint32_t _elen, uint32_t _vlen)
        : VectorMemMicroInst(mnem, _machInst, __opClass, _microVl, _microIdx,
                             _offset, _elen, _vlen),
          regIdx(_regIdx),
          has_rs2(_has_rs2)
    {}

    std::string
    generateDisassembly(Addr pc,
                        const loader::SymbolTable *symtab) const override;
};

class VlIndexMacroInst : public VectorMemMacroInst
{
  protected:
    VlIndexMacroInst(const char *mnem, ExtMachInst _machInst,
                     OpClass __opClass, uint32_t _elen, uint32_t _vlen)
        : VectorMemMacroInst(mnem, _machInst, __opClass, _elen, _vlen)
    {}

    std::string
    generateDisassembly(Addr pc,
                        const loader::SymbolTable *symtab) const override;
};

class VlIndexMicroInst : public VectorMemMicroInst
{
  protected:
    uint32_t vdRegIdx;
    uint32_t vdElemIdx;
    uint32_t vs2RegIdx;
    uint32_t vs2ElemIdx;
    VlIndexMicroInst(const char *mnem, ExtMachInst _machInst,
                     OpClass __opClass, uint32_t _vdRegIdx,
                     uint32_t _vdElemIdx, uint32_t _vs2RegIdx,
                     uint32_t _vs2ElemIdx, uint32_t _elen, uint32_t _vlen)
        : VectorMemMicroInst(mnem, _machInst, __opClass, 1, 0, 0, _elen,
                             _vlen),
          vdRegIdx(_vdRegIdx),
          vdElemIdx(_vdElemIdx),
          vs2RegIdx(_vs2RegIdx),
          vs2ElemIdx(_vs2ElemIdx)
    {}

    std::string
    generateDisassembly(Addr pc,
                        const loader::SymbolTable *symtab) const override;
};

class VsIndexMacroInst : public VectorMemMacroInst
{
  protected:
    VsIndexMacroInst(const char *mnem, ExtMachInst _machInst,
                     OpClass __opClass, uint32_t _elen, uint32_t _vlen)
        : VectorMemMacroInst(mnem, _machInst, __opClass, _elen, _vlen)
    {}

    std::string
    generateDisassembly(Addr pc,
                        const loader::SymbolTable *symtab) const override;
};

class VsIndexMicroInst : public VectorMemMicroInst
{
  protected:
    uint32_t vs3RegIdx;
    uint32_t vs3ElemIdx;
    uint32_t vs2RegIdx;
    uint32_t vs2ElemIdx;
    VsIndexMicroInst(const char *mnem, ExtMachInst _machInst,
                     OpClass __opClass, uint32_t _vs3RegIdx,
                     uint32_t _vs3ElemIdx, uint32_t _vs2RegIdx,
                     uint32_t _vs2ElemIdx, uint32_t _elen, uint32_t _vlen)
        : VectorMemMicroInst(mnem, _machInst, __opClass, 1, 0, 0, _elen,
                             _vlen),
          vs3RegIdx(_vs3RegIdx),
          vs3ElemIdx(_vs3ElemIdx),
          vs2RegIdx(_vs2RegIdx),
          vs2ElemIdx(_vs2ElemIdx)
    {}

    std::string
    generateDisassembly(Addr pc,
                        const loader::SymbolTable *symtab) const override;
};

class VMvWholeMacroInst : public VectorArithMacroInst
{
  protected:
    VMvWholeMacroInst(const char *mnem, ExtMachInst _machInst,
                      OpClass __opClass, uint32_t _elen, uint32_t _vlen)
        : VectorArithMacroInst(mnem, _machInst, __opClass, _elen, _vlen)
    {}

    std::string
    generateDisassembly(Addr pc,
                        const loader::SymbolTable *symtab) const override;
};

class VMvWholeMicroInst : public VectorArithMicroInst
{
  protected:
    VMvWholeMicroInst(const char *mnem, ExtMachInst _machInst,
                      OpClass __opClass, uint32_t _microVl, uint32_t _microIdx,
                      uint32_t _elen, uint32_t _vlen)
        : VectorArithMicroInst(mnem, _machInst, __opClass, _microVl, _microIdx,
                               _elen, _vlen)
    {}

    std::string
    generateDisassembly(Addr pc,
                        const loader::SymbolTable *symtab) const override;
};

class VMaskMergeMicroInst : public VectorArithMicroInst
{
  private:
    RegId srcRegIdxArr[NumVecInternalRegs];
    RegId destRegIdxArr[1];

  public:
    size_t elemSize;
    VMaskMergeMicroInst(ExtMachInst extMachInst, uint8_t _dstReg,
                        uint8_t _numSrcs, uint32_t _elen, uint32_t _vlen,
                        size_t _elemSize);
    Fault execute(ExecContext *, trace::InstRecord *) const override;
    std::string
    generateDisassembly(Addr, const loader::SymbolTable *) const override;
};

class VxsatMicroInst : public VectorArithMicroInst
{
  private:
    bool *vxsat;

  public:
    VxsatMicroInst(bool *Vxsat, ExtMachInst extMachInst, uint32_t _elen,
                   uint32_t _vlen)
        : VectorArithMicroInst("vxsat_micro", extMachInst, SimdMiscOp, 0, 0,
                               _elen, _vlen)
    {
        vxsat = Vxsat;
    }
    Fault execute(ExecContext *, trace::InstRecord *) const override;
    std::string
    generateDisassembly(Addr, const loader::SymbolTable *) const override;
};

class VlFFTrimVlMicroOp : public VectorMicroInst
{
  private:
    RegId srcRegIdxArr[8];
    RegId destRegIdxArr[0];
    std::vector<StaticInstPtr> &microops;

  public:
    VlFFTrimVlMicroOp(ExtMachInst _machInst, uint32_t _microVl,
                      uint32_t _microIdx, uint32_t _elen, uint32_t _vlen,
                      std::vector<StaticInstPtr> &_microops);
    uint32_t calcVl() const;
    Fault execute(ExecContext *, trace::InstRecord *) const override;
    std::unique_ptr<PCStateBase> branchTarget(ThreadContext *) const override;
    std::string
    generateDisassembly(Addr, const loader::SymbolTable *) const override;
};

class VlSegMacroInst : public VectorMemMacroInst
{
  protected:
    VlSegMacroInst(const char *mnem, ExtMachInst _machInst, OpClass __opClass,
                   uint32_t _elen, uint32_t _vlen)
        : VectorMemMacroInst(mnem, _machInst, __opClass, _elen, _vlen)
    {}

    std::string
    generateDisassembly(Addr pc,
                        const loader::SymbolTable *symtab) const override;
};

class VlSegMicroInst : public VectorMicroInst
{
  protected:
    Request::Flags memAccessFlags;
    uint8_t regIdx;
    mutable bool trimVl;
    mutable uint32_t faultIdx;

    VlSegMicroInst(const char *mnem, ExtMachInst _machInst, OpClass __opClass,
                   uint32_t _microVl, uint32_t _microIdx,
                   uint32_t _numMicroops, uint32_t _field, uint32_t _numFields,
                   uint32_t _elen, uint32_t _vlen)
        : VectorMicroInst(mnem, _machInst, __opClass, _microVl, _microIdx,
                          _elen, _vlen),
          trimVl(false),
          faultIdx(_microVl)
    {
        this->flags[IsLoad] = true;
    }

    std::string
    generateDisassembly(Addr pc,
                        const loader::SymbolTable *symtab) const override;
};

class VlSegDeIntrlvMicroInst : public VectorArithMicroInst
{
  private:
    RegId srcRegIdxArr[NumVecInternalRegs];
    RegId destRegIdxArr[1];
    uint32_t numSrcs;
    uint32_t numMicroops;
    uint32_t field;
    uint32_t sizeOfElement;
    uint32_t micro_vl;

  public:
    VlSegDeIntrlvMicroInst(ExtMachInst extMachInst, uint32_t _micro_vl,
                           uint32_t _dstReg, uint32_t _numSrcs,
                           uint32_t _microIdx, uint32_t _numMicroops,
                           uint32_t _field, uint32_t _elen, uint32_t _vlen,
                           uint32_t _sizeOfElement);

    Fault execute(ExecContext *, trace::InstRecord *) const override;

    std::string
    generateDisassembly(Addr, const loader::SymbolTable *) const override;
};

class VsSegMacroInst : public VectorMemMacroInst
{
  protected:
    VsSegMacroInst(const char *mnem, ExtMachInst _machInst, OpClass __opClass,
                   uint32_t _elen, uint32_t _vlen)
        : VectorMemMacroInst(mnem, _machInst, __opClass, _elen, _vlen)
    {}

    std::string
    generateDisassembly(Addr pc,
                        const loader::SymbolTable *symtab) const override;
};

class VsSegMicroInst : public VectorMicroInst
{
  protected:
    Request::Flags memAccessFlags;
    uint8_t regIdx;

    VsSegMicroInst(const char *mnem, ExtMachInst _machInst, OpClass __opClass,
                   uint32_t _microVl, uint32_t _microIdx,
                   uint32_t _numMicroops, uint32_t _field, uint32_t _numFields,
                   uint32_t _elen, uint32_t _vlen)
        : VectorMicroInst(mnem, _machInst, __opClass, _microVl, _microIdx,
                          _elen, _vlen)
    {
        this->flags[IsStore] = true;
    }

    std::string
    generateDisassembly(Addr pc,
                        const loader::SymbolTable *symtab) const override;
};

class VsSegIntrlvMicroInst : public VectorArithMicroInst
{
  private:
    RegId srcRegIdxArr[NumVecInternalRegs];
    RegId destRegIdxArr[1];
    uint32_t numSrcs;
    uint32_t numMicroops;
    uint32_t field;
    uint32_t sizeOfElement;
    uint32_t micro_vl;

  public:
    VsSegIntrlvMicroInst(ExtMachInst extMachInst, uint32_t _micro_vl,
                         uint32_t _dstReg, uint32_t _numSrcs,
                         uint32_t _microIdx, uint32_t _numMicroops,
                         uint32_t _field, uint32_t _elen, uint32_t _vlen,
                         uint32_t _sizeOfElement);

    Fault execute(ExecContext *, trace::InstRecord *) const override;

    std::string
    generateDisassembly(Addr, const loader::SymbolTable *) const override;
};

class VCpyVsMicroInst : public VectorArithMicroInst
{
  private:
    RegId srcRegIdxArr[1];
    RegId destRegIdxArr[1];

  public:
    VCpyVsMicroInst(ExtMachInst _machInst, uint32_t _microIdx,
                    uint8_t _vsRegIdx, uint32_t _elen, uint32_t _vlen);
    Fault execute(ExecContext *, trace::InstRecord *) const override;
    std::string
    generateDisassembly(Addr pc,
                        const loader::SymbolTable *symtab) const override;
};

class VPinVdMicroInst : public VectorArithMicroInst
{
  private:
    RegId srcRegIdxArr[1];
    RegId destRegIdxArr[1];
    const bool hasVdOffset;

  public:
    VPinVdMicroInst(ExtMachInst _machInst, uint32_t _microIdx,
                    uint32_t _numVdPins, uint32_t _elen, uint32_t _vlen,
                    bool _hasVdOffset = false);
    Fault execute(ExecContext *, trace::InstRecord *) const override;
    std::string
    generateDisassembly(Addr pc,
                        const loader::SymbolTable *symtab) const override;
};

} // namespace RiscvISA
} // namespace gem5

#endif // __ARCH_RISCV_INSTS_VECTOR_HH__
