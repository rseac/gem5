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
        vrf.removeRead(it->srcA);
        vrf.removeRead(it->srcB);
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
AraCoprocessor::pushInstruction(StaticInstPtr inst, ThreadContext* tc,
    InstSeqNum seq_num)
{
    DPRINTF(AraCoprocessor, "Received vector instruction: %s (seq %llu)\n",
            inst->getName(), seq_num);
    commandQueue.push({inst, tc, seq_num});
    
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
        ThreadContext* tc = commandQueue.front().tc;
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

        // Track in-flight accesses (cleared on completion)
        vrf.addRead(mock_srcA);
        vrf.addRead(mock_srcB);
        if (hasVrfDest && !destIsOwnSource) {
            vrf.addWrite(mock_dest);
        }

        unsigned active_vl = 16;
        unsigned sew_bits = 64;

        if (tc) {
            active_vl = tc->readMiscReg(RiscvISA::MISCREG_VL);
            uint64_t vtype = tc->readMiscReg(RiscvISA::MISCREG_VTYPE);
            unsigned vsew = (vtype >> 3) & 0x7;
            sew_bits = 8 << vsew;
            if (active_vl == 0) active_vl = 1;
            DPRINTF(AraCoprocessor, "Read CSRs for %s. VL: %u, SEW: %u\n",
                    inst->getName(), active_vl, sew_bits);
        }

        unsigned total_bits = active_vl * sew_bits;
        unsigned bits_per_cycle = numLanes * datapathWidth;
        unsigned chunks_needed = (total_bits + bits_per_cycle - 1) / bits_per_cycle;

        std::string instName = inst->getName();
        if (instName.find("div") != std::string::npos || instName.find("sqrt") != std::string::npos) {
            chunks_needed *= 15; // fpnew iterative divider average penalty
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
        if (lane.tick(newlyCompleted)) {
            active = true;
        }
    }

    for (auto seq : newlyCompleted) {
        DPRINTF(AraCoprocessor, "Ara Vector Instruction Completed (seq %llu)\n", seq);
        markCompleted(seq);
    }
    
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
