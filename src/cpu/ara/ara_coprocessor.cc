#include "cpu/ara/ara_coprocessor.hh"

#include "base/trace.hh"
#include "debug/AraCoprocessor.hh"
#include "cpu/thread_context.hh"
#include "arch/riscv/isa.hh"

namespace gem5
{

AraCoprocessor::AraCoprocessor(const AraCoprocessorParams &params) :
    ClockedObject(params),
    numLanes(params.num_lanes),
    vlen(params.vlen),
    datapathWidth(params.datapath_width),
    axiDataWidth(params.axi_data_width),
    vrf(32, params.vlen), // Standard RISC-V has 32 vector registers
    tickEvent([this]{ processTick(); }, name() + ".tickEvent"),
    dcachePort(name() + ".dcache_port", this)
{
    // Initialize the vector lanes
    for (unsigned i = 0; i < numLanes; i++) {
        lanes.emplace_back(i, datapathWidth, &vrf);
    }
}

Port &
AraCoprocessor::getPort(const std::string &if_name, PortID idx)
{
    if (if_name == "dcache_port") {
        return dcachePort;
    }
    return ClockedObject::getPort(if_name, idx);
}

bool
AraCoprocessor::VectorPort::recvTimingResp(PacketPtr pkt)
{
    return coprocessor->handleMemoryResponse(pkt);
}

void
AraCoprocessor::VectorPort::recvReqRetry()
{
    coprocessor->retrySendMemoryRequest();
}

void
AraCoprocessor::retrySendMemoryRequest()
{
    panic_if(!blockedPkt, "recvReqRetry() with no blocked packet.");
    if (dcachePort.sendTimingReq(blockedPkt)) {
        DPRINTF(AraCoprocessor, "VLSU retry succeeded, request sent.\n");
        blockedPkt = nullptr;
        if (!tickEvent.scheduled())
            schedule(tickEvent, nextCycle());
    }
    // Otherwise stay blocked; another recvReqRetry() will follow.
}

bool
AraCoprocessor::handleMemoryResponse(PacketPtr pkt)
{
    DPRINTF(AraCoprocessor, "VLSU received AXI memory response for address %#x (Size: %u bytes)\n",
            pkt->getAddr(), pkt->getSize());

    auto *state = dynamic_cast<MemSenderState *>(pkt->popSenderState());
    panic_if(!state, "Vector memory response without an AraCoprocessor "
                      "sender state.");

    // Write data to the VRF (mocked) and mark the destination ready across
    // its whole width so dependent math/store instructions can proceed.
    for (unsigned byte = 0; byte < state->destBytes; byte++) {
        // vrf.registers[state->destReg][byte] = pkt->getConstPtr<uint8_t>()[byte];
        vrf.markReady(state->destReg, byte);
    }

    // The instruction that issued this request is now done: remove it from
    // the in-flight memory queue and signal the CPU it can commit. Matched
    // by seqNum, not StaticInstPtr - the same StaticInst is shared by every
    // dynamic occurrence of this instruction (e.g. every loop iteration),
    // so StaticInstPtr identity alone can't tell two in-flight iterations
    // apart.
    auto it = std::find_if(memoryQueue.begin(), memoryQueue.end(),
        [state](const InFlightInst &fi) { return fi.seqNum == state->seqNum; });
    if (it != memoryQueue.end()) {
        if (it->numSrcRegs > 0) vrf.removeRead(it->srcA);
        if (it->numSrcRegs > 1) vrf.removeRead(it->srcB);
        if (!it->destIsOwnSource) {
            vrf.removeWrite(it->dest);
        }
        memoryQueue.erase(it);
    }

    DPRINTF(AraCoprocessor, "Ara Vector Memory Instruction Completed: %s "
            "(seq %llu)\n", state->inst->getName(), state->seqNum);
    markCompleted(state->seqNum);

    delete state;
    delete pkt;

    // Wake up the coprocessor if it was completely stalled waiting on this memory
    if (!tickEvent.scheduled()) {
        schedule(tickEvent, nextCycle());
    }

    return true;
}

void
AraCoprocessor::init()
{
    ClockedObject::init();
    // Initialization code here
}

void
AraCoprocessor::startup()
{
    // Do not schedule tick until an instruction arrives
}

void
AraCoprocessor::pushInstruction(StaticInstPtr inst, uint32_t vl,
    uint64_t vtype, InstSeqNum seq_num)
{
    DPRINTF(AraCoprocessor, "Received vector instruction: %s (seq %llu)\n",
            inst->getName(), seq_num);
    commandQueue.push({inst, vl, vtype, seq_num});
    
    // Wake up the coprocessor if it's idle
    if (!tickEvent.scheduled()) {
        schedule(tickEvent, nextCycle());
    }
}

void
AraCoprocessor::processTick()
{
    DPRINTF(AraCoprocessor, "Ara Sequencer Tick. Queue: %lu\n", commandQueue.size());
    
    // 1. Issue Stage: Continuously pop from the Command Queue
    // In a full RTL model, we would check structural hazards (e.g., are the lane queues full?)
    // before popping. Here we assume our lane queues can buffer the in-flight instructions.
    // Stall issue entirely while a memory request is waiting on a port
    // retry: the RequestPort must not send another request until then.
    while (!commandQueue.empty() && !blockedPkt) {
        StaticInstPtr inst = commandQueue.front().inst;
        uint32_t cmd_vl = commandQueue.front().vl;
        uint64_t cmd_vtype = commandQueue.front().vtype;
        InstSeqNum seqNum = commandQueue.front().seqNum;

        unsigned mock_srcA = 0;
        unsigned mock_srcB = 0;
        unsigned mock_dest = 0;

        int vec_src_count = 0;
        for (int i = 0; i < inst->numSrcRegs(); i++) {
            if (inst->srcRegIdx(i).classValue() == gem5::VecRegClass || inst->srcRegIdx(i).classValue() == gem5::VecElemClass) {
                if (vec_src_count == 0) mock_srcA = inst->srcRegIdx(i).index();
                else if (vec_src_count == 1) mock_srcB = inst->srcRegIdx(i).index();
                vec_src_count++;
            }
        }

        bool hasVrfDest = false;
        for (int i = 0; i < inst->numDestRegs(); i++) {
            if (inst->destRegIdx(i).classValue() == gem5::VecRegClass || inst->destRegIdx(i).classValue() == gem5::VecElemClass) {
                mock_dest = inst->destRegIdx(i).index();
                hasVrfDest = true;
                break;
            }
        }

        bool destIsOwnSource = (mock_dest == mock_srcA || mock_dest == mock_srcB);

        // STALL ISSUE on WAW/WAR hazards. Stores never write VRF.
        if (hasVrfDest && !destIsOwnSource && vrf.hasHazard(mock_dest)) {
            DPRINTF(AraCoprocessor, "STALLING %s because of hazard on v%d! "
                    "pendingReads=%d, pendingWrites=%d\n",
                    inst->getName(), mock_dest,
                    vrf.getPendingReads(mock_dest), vrf.getPendingWrites(mock_dest));
            break;
        }

        commandQueue.pop();

        // Track in-flight accesses (cleared on completion). Gated by
        // vec_src_count (== numSrcRegs on the InFlightInst below) so this
        // exactly matches every removeRead() site - otherwise an
        // instruction with no real vector source (mock_srcA/B left at
        // their default of 0) still increments register 0's pending-read
        // count, which nothing ever decrements (removeRead is correctly
        // skipped for it), permanently "hazarding" register 0 - i.e. the
        // real v0 mask register - and deadlocking any later instruction
        // that legitimately writes it.
        if (vec_src_count > 0) vrf.addRead(mock_srcA);
        if (vec_src_count > 1) vrf.addRead(mock_srcB);
        if (hasVrfDest && !destIsOwnSource) {
            vrf.addWrite(mock_dest);
        }

        // Captured by the caller at this instruction's own issue time (see
        // the PendingCmd comment in ara_coprocessor.hh) instead of read
        // live here, where a backed-up commandQueue could pick up a later,
        // unrelated instruction's VL/VTYPE.
        unsigned active_vl = cmd_vl;
        unsigned vsew = (cmd_vtype >> 3) & 0x7;
        unsigned sew_bits = 8 << vsew;
        if (active_vl == 0) active_vl = 1;
        DPRINTF(AraCoprocessor, "Read CSRs for %s. VL: %u, SEW: %u\n",
                inst->getName(), active_vl, sew_bits);

        unsigned total_bits = active_vl * sew_bits;
        unsigned bits_per_cycle = numLanes * datapathWidth;
        unsigned chunks_needed = (total_bits + bits_per_cycle - 1) / bits_per_cycle;

        std::string instName = inst->getName();
        if (instName.find("div") != std::string::npos || instName.find("sqrt") != std::string::npos) {
            chunks_needed *= 15; // fpnew iterative divider average penalty
        }

        // TRIED AND REVERTED: a fixed per-instruction ALU latency here
        // (grounded in real Ara RTL VALU-ADD measurements of ~23-26 cycles
        // regardless of VL), even after excluding memory ops and
        // lightweight scalar/mask/control micro-ops, looked like a clear
        // win when tuned against matmul/axpy/spmv alone (avg MAPE 25.4% ->
        // 22.0% at a +12-cycle constant) but was a net regression across
        // the full 11-benchmark suite (18.3% -> 25.7% average): somier
        // (1.0% -> 29.0%), swaptions (2.1% -> 48.1%), lavaMD (18.8% ->
        // 70.6%), and blackscholes (35.9% -> 70.4%) all got dramatically
        // worse, more than offsetting real gains on matmul (19.2% -> 6.3%)
        // and jacobi-2d (38.1% -> 20.7%). The 3-benchmark tuning set was
        // not representative enough to validate a change like this - any
        // future attempt at a fixed per-instruction latency term needs to
        // be checked against the full benchmark suite before being kept,
        // not just the 2-3 benchmarks motivating it.

        // Indexed/gather ("xei": vluxei/vloxei/vsuxei/vsoxei) addresses are
        // genuinely unpredictable per element - each lane's address comes
        // from a data-dependent index, so real hardware can't do better
        // than servicing them close to one at a time. Plain strided access
        // ("lse"/"sse" without "xei": vlse/vsse) has a known, constant
        // stride computed once - real hardware can still pipeline/predict
        // these even though they can't coalesce into a single unit-stride
        // burst, so they're charged far less than genuine gather/scatter.
        // Both used to share one flat 4-cycles/element rate, which overcharged
        // every regular-stride kernel (e.g. matmul's column-major access to
        // its second matrix, lavaMD's struct-of-arrays neighbor loads) as if
        // it were random gather.
        bool isIndexed = instName.find("xei") != std::string::npos;
        bool isStrided = !isIndexed && (instName.find("lse") != std::string::npos ||
                                        instName.find("sse") != std::string::npos);
        if (isIndexed || isStrided) {
            // Recalibrated from real Ara RTL (Verilator) measurements on a
            // stride-8 VLSU microbenchmark: VL16 -> 85 cycles, VL256 -> 140
            // cycles, VL1024 -> 138 cycles. The flat active_vl*2 (strided) /
            // active_vl*4 (indexed) model this replaced predicted 512
            // cycles at VL256 - a ~3.6x overcharge - because real hardware
            // pipelines up to 4 outstanding per-element AXI requests
            // (VaddrgenInsnQueueDepth in ara/hardware/src/vlsu/addrgen.sv)
            // instead of serializing them, so cost is dominated by a fixed
            // AXI round-trip latency plus a small, saturating per-element
            // term rather than growing linearly forever. No solid evidence
            // indexed/gather is reliably worse than plain strided (a
            // secondary CoralNPU dataset showed them roughly equal), so
            // both use the same formula for now.
            constexpr unsigned STRIDE_FIXED_LATENCY = 64;
            constexpr unsigned STRIDE_SATURATION_VL = 256;
            constexpr double STRIDE_CYCLES_PER_ELEM = 0.3;
            unsigned charged_vl = std::min(active_vl, STRIDE_SATURATION_VL);
            chunks_needed = STRIDE_FIXED_LATENCY +
                (unsigned)(charged_vl * STRIDE_CYCLES_PER_ELEM);
            DPRINTF(AraCoprocessor, "Applying %s memory penalty to %s: %u cycles\n",
                    isIndexed ? "indexed/gather" : "strided", instName.c_str(), chunks_needed);
        }

        if (hasVrfDest && !destIsOwnSource) {
            for (unsigned byte = 0; byte < (active_vl * sew_bits / 8); byte++) {
                vrf.markPending(mock_dest, byte);
            }
        }

        // For stores: destIsOwnSource flag reused to suppress removeWrite()
        InFlightInst finst = {
            inst, seqNum, mock_srcA, mock_srcB, mock_dest, vec_src_count,
            sew_bits / 8 == 0 ? 1 : sew_bits / 8,
            chunks_needed,
            0,
            inst->isLoad() || inst->isStore(),
            destIsOwnSource || !hasVrfDest,
            hasVrfDest
        };

        if (finst.isMemory) {
            DPRINTF(AraCoprocessor, "Vector Memory Op issued to internal VLSU model.\n");
            memoryQueue.push_back(finst);
        } else {
            DPRINTF(AraCoprocessor, "Vector ALU Op issued to Lanes.\n");
            for (auto& lane : lanes) {
                lane.pushInstruction(finst);
            }
        }
    }
    
    // 2. Execute Stage: Tick all active lanes concurrently (Pipelined!)
    bool active = false;
    std::vector<InstSeqNum> newlyCompleted;
    for (auto& lane : lanes) {
        if (lane.tick(newlyCompleted, writebackQueue, curCycle())) {
            active = true;
        }
    }

    for (auto seq : newlyCompleted) {
        DPRINTF(AraCoprocessor, "Ara Vector Instruction Completed (seq %llu)\n", seq);
        markCompleted(seq);
    }
    

    // Process writebacks
    auto it = writebackQueue.begin();
    while (it != writebackQueue.end()) {
        if (curCycle() >= it->readyCycle) {
            vrf.markReady(it->destReg, it->byteIndex);
            active = true;
            it = writebackQueue.erase(it);
        } else {
            ++it;
        }
    }

    // A writeback entry with a still-future readyCycle (e.g. the 5-cycle
    // "vf" pipeline latency) correctly does nothing above, but that means
    // it never sets `active` either. If nothing else does so this cycle
    // (no lane progressed, memoryQueue empty), tickEvent is never
    // rescheduled and the whole coprocessor permanently freezes one or
    // more cycles short of when this exact entry would have unblocked
    // everything on its own - a real, reproduced deadlock, not a timeout.
    if (!writebackQueue.empty()) active = true;

    // Execute VLSU Queue (1 chunk per cycle)

    if (!memoryQueue.empty()) {
        active = true;
        InFlightInst& memInst = memoryQueue.front();
        memInst.chunksProcessed++;
        
        unsigned bytesDone = memInst.chunksProcessed * (axiDataWidth / 8);
        unsigned byteStart = bytesDone - (axiDataWidth / 8);
        if (!memInst.destIsOwnSource) {
            for (unsigned b = byteStart; b < bytesDone && b < (memInst.totalChunksNeeded * (axiDataWidth / 8)); b++) {
                vrf.markReady(memInst.dest, b);
            }
        }
        
        if (memInst.chunksProcessed >= memInst.totalChunksNeeded) {
            InstSeqNum completedInst = memInst.seqNum;
            if (memInst.numSrcRegs > 0) vrf.removeRead(memInst.srcA);
            if (memInst.numSrcRegs > 1) vrf.removeRead(memInst.srcB);
            if (memInst.hasVrfDest && !memInst.destIsOwnSource) {
                vrf.removeWrite(memInst.dest);
            }
            DPRINTF(AraCoprocessor, "Ara Vector Memory Instruction Completed (seq %llu)\n", completedInst);
            markCompleted(completedInst);
            memoryQueue.erase(memoryQueue.begin());
        }
    }
    
    if (active && !tickEvent.scheduled()) {
        schedule(tickEvent, nextCycle());
    }
}

} // namespace gem5
