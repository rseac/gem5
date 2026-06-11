#ifndef __MEM_RVV_EXT_HH__
#define __MEM_RVV_EXT_HH__

#include <memory>

#include "base/extensible.hh"
#include "cpu/op_class.hh"
#include "mem/request.hh"

namespace gem5
{

/**
 * Request extension carrying instruction context from the CPU down to the
 * memory system: the OpClass of the instruction that created the request
 * (e.g. SimdStridedLoad for a vlse64.v microop, MemRead for a scalar
 * load), and for strided vector accesses the rs2 register value (the
 * architectural byte stride).
 *
 * Attached via StaticInst::annotateMemRequest, called by the O3 LSQ when
 * the request is created (LSQ::LSQRequest::addReq), so it is visible
 * anywhere the request travels: caches, prefetchers, and Packet::print()
 * debug output.
 *
 * Requests not created by the LSQ (instruction fetches, writebacks,
 * cache-generated prefetches) carry no extension; consumers must handle
 * getExtension() returning nullptr.
 */
class RVVExtension : public Extension<Request, RVVExtension>
{
  public:
    explicit RVVExtension(OpClass op_class, int64_t rs2 = 0)
        : instType(op_class), rs2(rs2) {}

    std::unique_ptr<ExtensionBase>
    clone() const override
    {
        return std::make_unique<RVVExtension>(*this);
    }

    OpClass getInstType() const { return instType; }

    /**
     * rs2 register value. Only meaningful for strided accesses
     * (SimdStridedLoad / SimdStridedStore), where it is the byte stride;
     * 0 for everything else.
     */
    int64_t getRs2() const { return rs2; }

    /** Human-readable type name, e.g. "SimdStridedLoad". */
    const char *
    toString() const
    {
        return enums::OpClassStrings[instType];
    }

  private:
    OpClass instType;
    int64_t rs2;
};

} // namespace gem5

#endif // __MEM_RVV_EXT_HH__
