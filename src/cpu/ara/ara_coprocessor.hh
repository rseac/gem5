#include <algorithm>
#include <vector>
#ifndef __CPU_ARA_ARA_COPROCESSOR_HH__
#define __CPU_ARA_ARA_COPROCESSOR_HH__

#include <queue>
#include "cpu/inst_seq.hh"
#include "cpu/static_inst.hh"
#include "cpu/op_class.hh"
#include "mem/port.hh"
#include "mem/packet.hh"
#include "params/AraCoprocessor.hh"
#include "sim/clocked_object.hh"

namespace gem5
{
// ... (VectorRegisterFile, OperandRequester, VectorLane definitions) ...

class VectorRegisterFile {
  private:
    std::vector<std::vector<uint8_t>> registers;
    std::vector<std::vector<bool>> elementReady; // Tracks chunk readiness for chaining
    std::vector<int> pendingReads;
    std::vector<int> pendingWrites;
  public:
    VectorRegisterFile(unsigned num_regs, unsigned vlen_bits) {
        registers.resize(num_regs, std::vector<uint8_t>(vlen_bits / 8, 0));
        elementReady.resize(num_regs, std::vector<bool>(vlen_bits / 8, true));
        pendingReads.resize(num_regs, 0);
        pendingWrites.resize(num_regs, 0);
    }
    
    // mock_srcA/srcB/dest are derived heuristically from raw StaticInst
    // reg-class indices (see processTick()), which aren't guaranteed to
    // fall inside this model's fixed 0..31 vector-register slots (e.g. a
    // misc/CSR or otherwise-unexpected destination). Out-of-range access
    // is treated as untracked rather than indexing off the end of the
    // vector, which would otherwise be undefined behaviour/a segfault.
    bool inBounds(unsigned reg_idx, unsigned byte_idx) const {
        return reg_idx < elementReady.size() &&
            byte_idx < elementReady[reg_idx].size();
    }

    // Sequencer calls this at issue to reserve the destination chunks
    void markPending(unsigned reg_idx, unsigned byte_idx) {
        if (inBounds(reg_idx, byte_idx))
            elementReady[reg_idx][byte_idx] = false;
    }

    // Lanes call this during writeback to unblock dependent instructions immediately
    void markReady(unsigned reg_idx, unsigned byte_idx) {
        if (inBounds(reg_idx, byte_idx))
            elementReady[reg_idx][byte_idx] = true;
    }

    bool isReady(unsigned reg_idx, unsigned byte_idx) const {
        return !inBounds(reg_idx, byte_idx) || elementReady[reg_idx][byte_idx];
    }

    void addRead(unsigned reg_idx) {
        if (reg_idx < pendingReads.size()) { pendingReads[reg_idx]++; printf("addRead(%d) -> %d\n", reg_idx, pendingReads[reg_idx]); }
    }
    
    void removeRead(unsigned reg_idx) {
        if (reg_idx < pendingReads.size()) { pendingReads[reg_idx]--; printf("removeRead(%d) -> %d\n", reg_idx, pendingReads[reg_idx]); }
    }
    
    void addWrite(unsigned reg_idx) {
        if (reg_idx < pendingWrites.size()) pendingWrites[reg_idx]++;
    }
    
    void removeWrite(unsigned reg_idx) {
        if (reg_idx < pendingWrites.size()) pendingWrites[reg_idx]--;
    }
    
    int getPendingReads(unsigned reg_idx) const { return pendingReads[reg_idx]; }
    int getPendingWrites(unsigned reg_idx) const { return pendingWrites[reg_idx]; }
    bool hasHazard(unsigned reg_idx) const {
        if (reg_idx >= pendingWrites.size()) return false;
        return pendingWrites[reg_idx] > 0 || pendingReads[reg_idx] > 0;
    }
};

class OperandRequester {
  private:
    unsigned srcReg;
    unsigned currentByteIdx;
    VectorRegisterFile* vrf;
  public:
    OperandRequester(VectorRegisterFile* _vrf) : srcReg(0), currentByteIdx(0), vrf(_vrf) {}
    
    void setSource(unsigned reg) { srcReg = reg; currentByteIdx = 0; }
    
    // The core of Ara's chaining: checking if the specific chunk is ready
    bool canFetchChunk() { return vrf->isReady(srcReg, currentByteIdx); }
    
    void fetchChunk(unsigned datapath_bytes) { currentByteIdx += datapath_bytes; }
    unsigned getCurrentByteIdx() const { return currentByteIdx; }
    unsigned getCurrentSrcReg() const { return srcReg; }
};

struct InFlightInst {
    StaticInstPtr inst;
    // gem5 caches/shares one StaticInst object across every dynamic
    // occurrence of the same instruction (e.g. every iteration of a
    // loop's vle64.v), so StaticInstPtr identity alone cannot tell two
    // in-flight loop iterations apart. seqNum is the CPU's unique
    // per-dynamic-instance id (MinorDynInst::id.execSeqNum) and is what
    // completion tracking must key on.
    InstSeqNum seqNum;
    unsigned srcA, srcB, dest;
    int numSrcRegs;
    unsigned sewBytes;
    unsigned totalChunksNeeded;
    unsigned chunksProcessed;
    bool isMemory; // Is this a VLSU op?
    bool destIsOwnSource;
    bool hasVrfDest;
};

class VectorLane {
  private:
    unsigned laneId;
    unsigned datapathBytes;
    VectorRegisterFile* vrf;

    // Pipelined Instruction Queues for this lane
    std::queue<InFlightInst> instQueue;
    
    // Active Execution State
    OperandRequester reqA;
    OperandRequester reqB;

  public:
    VectorLane(unsigned id, unsigned width_bits, VectorRegisterFile* _vrf) 
        : laneId(id), datapathBytes(width_bits / 8), vrf(_vrf), reqA(_vrf), reqB(_vrf) {}

    void pushInstruction(InFlightInst finst) {
        instQueue.push(finst);
        // If this is the first instruction, setup the requesters immediately
        if (instQueue.size() == 1) {
            reqA.setSource(finst.srcA);
            reqB.setSource(finst.srcB);
        }
    }

    bool isQueueEmpty() const { return instQueue.empty(); }

    bool tick(std::vector<InstSeqNum>& completedInsts) {
        if (instQueue.empty()) return false;

        InFlightInst& activeInst = instQueue.front();

        // Instruction-level RAW gate: stall if the source register still has
        // a pending write from a prior in-flight instruction.  The old
        // byte-granularity canFetchChunk() gate was designed for element-level
        // chaining across micro-ops, but now that we collapse all micro-ops of
        // a macro-instruction into one, the intermediate per-byte markReady()
        // calls never happen and the lane stalls forever.  A coarse
        // pendingWrites==0 check correctly models instruction-level RAW
        // without relying on byte readiness markers.
        // Restore element-level chaining:
        unsigned bytesNeeded = (activeInst.chunksProcessed + 1) * datapathBytes;
        unsigned totalBytes = activeInst.totalChunksNeeded * datapathBytes;
        if (bytesNeeded > totalBytes) bytesNeeded = totalBytes;
        
        bool srcA_ready = true;
        bool srcB_ready = true;
        
        // We only care if it's currently pending write from ANOTHER instruction
        for (unsigned b = 0; b < bytesNeeded; b++) {
            if (!vrf->isReady(activeInst.srcA, b)) srcA_ready = false;
            if (!vrf->isReady(activeInst.srcB, b)) srcB_ready = false;
        }

        if (srcA_ready && srcB_ready) {
            activeInst.chunksProcessed++;

            // Mark dest bytes ready as they're produced, so downstream
            // instructions in the lane queue can observe writeback.
            // All 4 lanes run in lockstep, so let Lane 0 mark the full 32 bytes for the whole cycle
            if (laneId == 0 && activeInst.hasVrfDest && !activeInst.destIsOwnSource) {
                // datapathBytes is e.g. 8 bytes per lane, 4 lanes = 32 bytes per cycle total
                unsigned bytesPerCycle = datapathBytes * 4; 
                unsigned bytesDone = activeInst.chunksProcessed * bytesPerCycle;
                unsigned byteStart = bytesDone - bytesPerCycle;
                for (unsigned b = byteStart; b < bytesDone; b++) {
                    vrf->markReady(activeInst.dest, b);
                }
            }

            if (activeInst.chunksProcessed >= activeInst.totalChunksNeeded) {
                InstSeqNum completedInst = activeInst.seqNum;
                if (laneId == 0) {
                    if (activeInst.numSrcRegs > 0) vrf->removeRead(activeInst.srcA);
                    if (activeInst.numSrcRegs > 1) vrf->removeRead(activeInst.srcB);
                    if (activeInst.hasVrfDest && !activeInst.destIsOwnSource) {
                        vrf->removeWrite(activeInst.dest);
                    }
                }
                instQueue.pop();
                if (!instQueue.empty()) {
                    reqA.setSource(instQueue.front().srcA);
                    reqB.setSource(instQueue.front().srcB);
                }
                if (laneId == 0) completedInsts.push_back(completedInst);
            }

            return true;
        }
        return false;
    }
};

class AraCoprocessor : public ClockedObject
{
  private:
    unsigned numLanes;
    unsigned vlen;
    unsigned datapathWidth;
    unsigned axiDataWidth;

    VectorRegisterFile vrf;
    std::vector<VectorLane> lanes;

    struct PendingCmd { StaticInstPtr inst; ThreadContext *tc; InstSeqNum seqNum; };
    std::queue<PendingCmd> commandQueue;

    // Track active memory instructions separately to model decoupled VLSU
    std::vector<InFlightInst> memoryQueue;

    // A request the port declined (sendTimingReq() returned false); held
    // here until recvReqRetry() lets us resend it. While set, no further
    // memory requests are issued (a RequestPort may not send again until
    // its retry fires), so this instruction's InFlightInst stays queued
    // in memoryQueue rather than being dropped.
    PacketPtr blockedPkt = nullptr;

    EventFunctionWrapper tickEvent;

    void processTick();
    void retrySendMemoryRequest();

    // Carried on an in-flight AXI packet so the response can complete the
    // instruction that issued it (and unblock its destination register)
    // instead of guessing.
    struct MemSenderState : public Packet::SenderState
    {
        StaticInstPtr inst;
        InstSeqNum seqNum;
        unsigned destReg;
        unsigned destBytes;

        MemSenderState(StaticInstPtr _inst, InstSeqNum _seqNum,
                       unsigned _destReg, unsigned _destBytes) :
            inst(_inst), seqNum(_seqNum), destReg(_destReg),
            destBytes(_destBytes)
        {}
    };

    // Phase 5: Vector Load/Store Unit Port
    class VectorPort : public RequestPort
    {
      private:
        AraCoprocessor *coprocessor;
      public:
        VectorPort(const std::string& name, AraCoprocessor *owner) :
            RequestPort(name, owner), coprocessor(owner) {}
      protected:
        bool recvTimingResp(PacketPtr pkt) override;
        void recvReqRetry() override;
    };

    VectorPort dcachePort;

  public:
    bool handleMemoryResponse(PacketPtr pkt);
    
    Port &getPort(const std::string &if_name, PortID idx=InvalidPortID) override;
    AraCoprocessor(const AraCoprocessorParams &params);

    void init() override;
    void startup() override;

    // Interface for the host CPU to offload vector instructions. seq_num
    // is the issuing MinorDynInst's unique id (id.execSeqNum) - see the
    // InFlightInst::seqNum comment for why StaticInstPtr alone can't
    // identify which dynamic occurrence this is.
    void pushInstruction(StaticInstPtr inst, ThreadContext* tc,
        InstSeqNum seq_num);

    // Scalar-Vector synchronization, keyed by seqNum (see above).
    std::vector<InstSeqNum> completedInstructions;

    bool hasCompleted(InstSeqNum seq_num) {
        auto it = std::find(completedInstructions.begin(), completedInstructions.end(), seq_num);
        return it != completedInstructions.end();
    }

    void markCommitted(InstSeqNum seq_num) {
        auto it = std::find(completedInstructions.begin(), completedInstructions.end(), seq_num);
        if (it != completedInstructions.end()) {
            completedInstructions.erase(it);
        }
    }

    void markCompleted(InstSeqNum seq_num) {
        completedInstructions.push_back(seq_num);
    }

    bool isQueueEmpty() const {
        if (!commandQueue.empty() || !memoryQueue.empty()) return false;
        for (const auto& lane : lanes) {
            if (!lane.isQueueEmpty()) return false;
        }
        return true;
    }
};

} // namespace gem5

#endif // __CPU_ARA_ARA_COPROCESSOR_HH__
