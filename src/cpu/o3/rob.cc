/*
 * Copyright (c) 2012 ARM Limited
 * All rights reserved
 *
 * The license below extends only to copyright in the software and shall
 * not be construed as granting a license to any other intellectual
 * property including but not limited to intellectual property relating
 * to a hardware implementation of the functionality of the software
 * licensed hereunder.  You may use the software subject to the license
 * terms below provided that you ensure that this notice is replicated
 * unmodified and in its entirety in all distributions of the software,
 * modified or unmodified, in source code or in binary form.
 *
 * Copyright (c) 2004-2006 The Regents of The University of Michigan
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

#include "cpu/o3/rob.hh"

#include <list>

#include "base/logging.hh"
#include "cpu/o3/dyn_inst.hh"
#include "cpu/o3/limits.hh"
#include "debug/Fetch.hh"
#include "debug/ROB.hh"
#include "params/BaseO3CPU.hh"
#include "debug/JY.hh"

namespace gem5
{

namespace o3
{

ROB::ROB(CPU *_cpu, const BaseO3CPUParams &params)
    : robPolicy(params.smtROBPolicy),
      cpu(_cpu),
      numEntries(params.numROBEntries),
      squashWidth(params.squashWidth),
      numInstsInROB(0),
      numThreads(params.numThreads),
      stats(_cpu)
{
    //Figure out rob policy
    if (robPolicy == SMTQueuePolicy::Dynamic) {
        //Set Max Entries to Total ROB Capacity
        for (ThreadID tid = 0; tid < numThreads; tid++) {
            maxEntries[tid] = numEntries;
        }

    } else if (robPolicy == SMTQueuePolicy::Partitioned) {
        DPRINTF(Fetch, "ROB sharing policy set to Partitioned\n");

        //@todo:make work if part_amt doesnt divide evenly.
        int part_amt = numEntries / numThreads;

        //Divide ROB up evenly
        for (ThreadID tid = 0; tid < numThreads; tid++) {
            maxEntries[tid] = part_amt;
        }

    } else if (robPolicy == SMTQueuePolicy::Threshold) {
        DPRINTF(Fetch, "ROB sharing policy set to Threshold\n");

        int threshold =  params.smtROBThreshold;;

        //Divide up by threshold amount
        for (ThreadID tid = 0; tid < numThreads; tid++) {
            maxEntries[tid] = threshold;
        }
    }

    for (ThreadID tid = numThreads; tid < MaxThreads; tid++) {
        maxEntries[tid] = 0;
    }

    resetState();
}

void
ROB::resetState()
{
    for (ThreadID tid = 0; tid  < MaxThreads; tid++) {
        threadEntries[tid] = 0;
        squashIt[tid] = instList[tid].end();
        squashedSeqNum[tid] = 0;
        doneSquashing[tid] = true;
    }
    numInstsInROB = 0;

    // Initialize the "universal" ROB head & tail point to invalid
    // pointers
    head = instList[0].end();
    tail = instList[0].end();
}

std::string
ROB::name() const
{
    return cpu->name() + ".rob";
}

void
ROB::setActiveThreads(std::list<ThreadID> *at_ptr)
{
    DPRINTF(ROB, "Setting active threads list pointer.\n");
    activeThreads = at_ptr;
}

void
ROB::drainSanityCheck() const
{
    for (ThreadID tid = 0; tid  < numThreads; tid++)
        assert(instList[tid].empty());
    assert(isEmpty());
}

void
ROB::takeOverFrom()
{
    resetState();
}

void
ROB::resetEntries()
{
    if (robPolicy != SMTQueuePolicy::Dynamic || numThreads > 1) {
        auto active_threads = activeThreads->size();

        for (ThreadID tid : *activeThreads) {
            if (robPolicy == SMTQueuePolicy::Partitioned) {
                maxEntries[tid] = numEntries / active_threads;
            } else if (robPolicy == SMTQueuePolicy::Threshold &&
                       active_threads == 1) {
                maxEntries[tid] = numEntries;
            }
        }
    }
}

int
ROB::entryAmount(ThreadID num_threads)
{
    if (robPolicy == SMTQueuePolicy::Partitioned) {
        return numEntries / num_threads;
    } else {
        return 0;
    }
}

int
ROB::countInsts()
{
    int total = 0;

    for (ThreadID tid = 0; tid < numThreads; tid++)
        total += countInsts(tid);

    return total;
}

size_t
ROB::countInsts(ThreadID tid)
{
    return instList[tid].size();
}

void
ROB::insertInst(const DynInstPtr &inst)
{
    assert(inst);

    stats.writes++;

    DPRINTF(ROB, "Adding inst PC %s to the ROB.\n", inst->pcState());

    assert(numInstsInROB != numEntries);

    ThreadID tid = inst->threadNumber;

    // TAINT LIFECYCLE: destination register tainted
    inst->setDestTaint(inst->isAccess() || inst->isArgsTainted());

    instList[tid].push_back(inst);

    //Set Up head iterator if this is the 1st instruction in the ROB
    if (numInstsInROB == 0) {
        head = instList[tid].begin();
        assert((*head) == inst);
    }

    //Must Decrement for iterator to actually be valid  since __.end()
    //actually points to 1 after the last inst
    tail = instList[tid].end();
    tail--;

    inst->setInROB();

    ++numInstsInROB;
    ++threadEntries[tid];

    assert((*tail) == inst);

    DPRINTF(ROB, "[tid:%i] Now has %d instructions.\n", tid,
            threadEntries[tid]);
}

void
ROB::retireHead(ThreadID tid)
{
    stats.writes++;

    assert(numInstsInROB > 0);

    // Get the head ROB instruction by copying it and remove it from the list
    InstIt head_it = instList[tid].begin();

    DynInstPtr head_inst = std::move(*head_it);
    instList[tid].erase(head_it);

    assert(head_inst->readyToCommit());

    DPRINTF(ROB, "[tid:%i] Retiring head instruction, "
            "instruction PC %s, [sn:%llu]\n", tid, head_inst->pcState(),
            head_inst->seqNum);

    --numInstsInROB;
    --threadEntries[tid];

    head_inst->clearInROB();
    head_inst->setCommitted();

    //Update "Global" Head of ROB
    updateHead();

    // @todo: A special case is needed if the instruction being
    // retired is the only instruction in the ROB; otherwise the tail
    // iterator will become invalidated.
    cpu->removeFrontInst(head_inst);
}

bool
ROB::isHeadReady(ThreadID tid)
{
    stats.reads++;
    if (threadEntries[tid] != 0) {
        return instList[tid].front()->readyToCommit();
    }

    return false;
}

bool
ROB::canCommit()
{
    //@todo: set ActiveThreads through ROB or CPU
    for (ThreadID tid : *activeThreads) {
        if (isHeadReady(tid)) {
            return true;
        }
    }

    return false;
}

unsigned
ROB::numFreeEntries()
{
    return numEntries - numInstsInROB;
}

unsigned
ROB::numFreeEntries(ThreadID tid)
{
    return maxEntries[tid] - threadEntries[tid];
}

void
ROB::doSquash(ThreadID tid)
{
    stats.writes++;
    DPRINTF(ROB, "[tid:%i] Squashing instructions until [sn:%llu].\n",
            tid, squashedSeqNum[tid]);

    assert(squashIt[tid] != instList[tid].end());

    if ((*squashIt[tid])->seqNum < squashedSeqNum[tid]) {
        DPRINTF(ROB, "[tid:%i] Done squashing instructions.\n",
                tid);

        squashIt[tid] = instList[tid].end();

        doneSquashing[tid] = true;
        return;
    }

    bool robTailUpdate = false;

    unsigned int numInstsToSquash = squashWidth;

    // If the CPU is exiting, squash all of the instructions
    // it is told to, even if that exceeds the squashWidth.
    // Set the number to the number of entries (the max).
    if (cpu->isThreadExiting(tid))
    {
        numInstsToSquash = numEntries;
    }

    for (int numSquashed = 0;
         numSquashed < numInstsToSquash &&
         squashIt[tid] != instList[tid].end() &&
         (*squashIt[tid])->seqNum > squashedSeqNum[tid];
         ++numSquashed)
    {
        DPRINTF(ROB, "[tid:%i] Squashing instruction PC %s, seq num %i.\n",
                (*squashIt[tid])->threadNumber,
                (*squashIt[tid])->pcState(),
                (*squashIt[tid])->seqNum);

        // Mark the instruction as squashed, and ready to commit so that
        // it can drain out of the pipeline.
        (*squashIt[tid])->setSquashed();

        (*squashIt[tid])->hasPendingSquash(false);

        (*squashIt[tid])->setCanCommit();


        if (squashIt[tid] == instList[tid].begin()) {
            DPRINTF(ROB, "Reached head of instruction list while "
                    "squashing.\n");

            squashIt[tid] = instList[tid].end();

            doneSquashing[tid] = true;

            return;
        }

        InstIt tail_thread = instList[tid].end();
        tail_thread--;

        if ((*squashIt[tid]) == (*tail_thread))
            robTailUpdate = true;

        squashIt[tid]--;
    }


    // Check if ROB is done squashing.
    if ((*squashIt[tid])->seqNum <= squashedSeqNum[tid]) {
        DPRINTF(ROB, "[tid:%i] Done squashing instructions.\n",
                tid);

        squashIt[tid] = instList[tid].end();

        doneSquashing[tid] = true;
    }

    if (robTailUpdate) {
        updateTail();
    }
}


void
ROB::updateHead()
{
    InstSeqNum lowest_num = 0;
    bool first_valid = true;

    // @todo: set ActiveThreads through ROB or CPU
    for (ThreadID tid : *activeThreads) {
        if (instList[tid].empty())
            continue;

        if (first_valid) {
            head = instList[tid].begin();
            lowest_num = (*head)->seqNum;
            first_valid = false;
            continue;
        }

        InstIt head_thread = instList[tid].begin();

        DynInstPtr head_inst = (*head_thread);

        assert(head_inst != 0);

        if (head_inst->seqNum < lowest_num) {
            head = head_thread;
            lowest_num = head_inst->seqNum;
        }
    }

    if (first_valid) {
        head = instList[0].end();
    }

}

void
ROB::updateTail()
{
    tail = instList[0].end();
    bool first_valid = true;

    for (ThreadID tid : *activeThreads) {
        if (instList[tid].empty()) {
            continue;
        }

        // If this is the first valid then assign w/out
        // comparison
        if (first_valid) {
            tail = instList[tid].end();
            tail--;
            first_valid = false;
            continue;
        }

        // Assign new tail if this thread's tail is younger
        // than our current "tail high"
        InstIt tail_thread = instList[tid].end();
        tail_thread--;

        if ((*tail_thread)->seqNum > (*tail)->seqNum) {
            tail = tail_thread;
        }
    }
}


void
ROB::squash(InstSeqNum squash_num, ThreadID tid)
{
    if (isEmpty(tid)) {
        DPRINTF(ROB, "Does not need to squash due to being empty "
                "[sn:%llu]\n",
                squash_num);

        return;
    }

    DPRINTF(ROB, "Starting to squash within the ROB.\n");

    robStatus[tid] = ROBSquashing;

    doneSquashing[tid] = false;

    squashedSeqNum[tid] = squash_num;

    if (!instList[tid].empty()) {
        InstIt tail_thread = instList[tid].end();
        tail_thread--;

        squashIt[tid] = tail_thread;

        doSquash(tid);
    }
}

const DynInstPtr&
ROB::readHeadInst(ThreadID tid)
{
    if (threadEntries[tid] != 0) {
        InstIt head_thread = instList[tid].begin();

        assert((*head_thread)->isInROB());

        return *head_thread;
    } else {
        return dummyInst;
    }
}

DynInstPtr
ROB::readTailInst(ThreadID tid)
{
    InstIt tail_thread = instList[tid].end();
    tail_thread--;

    return *tail_thread;
}

ROB::ROBStats::ROBStats(statistics::Group *parent)
  : statistics::Group(parent, "rob"),
    ADD_STAT(reads, statistics::units::Count::get(),
        "The number of ROB reads"),
    ADD_STAT(writes, statistics::units::Count::get(),
        "The number of ROB writes")
{
}

DynInstPtr
ROB::findInst(ThreadID tid, InstSeqNum squash_inst)
{
    for (InstIt it = instList[tid].begin(); it != instList[tid].end(); it++) {
        if ((*it)->seqNum == squash_inst) {
            return *it;
        }
    }
    return NULL;
}

/* **************************
 * [SafeSpec] update load insts state
 * isPrevInstsCompleted; isPrevBrsResolved
 * *************************/
void
ROB::updateVisibleState()
{
    for (ThreadID tid : *activeThreads) {
        bool prevInstsComplete = true;
        bool prevBrsResolved = true;
        bool prevInstsCommitted = true;
        bool prevBrsCommitted = true;

        for (const DynInstPtr &inst : instList[tid]) {
            if (inst->isSquashed())
                continue;

            if (!prevInstsComplete && !prevBrsResolved) {
                break;
            }

            if (prevInstsComplete)
                inst->setPrevInstsCompleted();
            if (prevBrsResolved)
                inst->setPrevBrsResolved();
            if (prevInstsCommitted)
                inst->setPrevInstsCommitted();
            if (prevBrsCommitted)
                inst->setPrevBrsCommitted();

            // Update prev control insts state
            if (inst->isControl()) {
                prevBrsCommitted = false;
                if (!inst->readyToCommit() || inst->getFault() != NoFault) {
                    prevBrsResolved = false;
                }
            }

            prevInstsCommitted = false;

            // Update prev insts state
            if (inst->isNonSpeculative() || inst->isStoreConditional() ||
                inst->isFullMemBarrier() ||
                inst->isWriteBarrier() || (inst->isLoad() && inst->strictlyOrdered()) )
            {
                // Some special instructions, directly set canCommit
                // when entering ROB
                prevInstsComplete = false;
            }

            if (!inst->readyToCommit() || inst->getFault() != NoFault) {
                prevInstsComplete = false;
            }

            // [Jiyong, Rutvik, SPT] Checking whether instructions have reached the VP
            if (cpu->spt) {
                bool unsquashableNow = (!cpu->isFuturistic && inst->isPrevBrsResolved()) ||
                                       ( cpu->isFuturistic && inst->isPrevInstsCompleted());
                inst->isUnsquashable(unsquashableNow);
            }
            else {
                inst->isUnsquashable(true);
            }
        }

        if (!cpu->disableUntaint)
            propagateUntaint(tid);
    }
}

void
ROB::propagateUntaint(ThreadID tid)
{
    // Registers are added to a queue that has a finite limit. In terms of priority:
    //   - The regs of older instructions are preferred to those of younger instructions.
    //   - The dest regs of an instruction are preferred to the src regs
    //   - The dest and src regs are added in order of index (0, 1, 2, ...)

    // phys reg, size, offset, the inst being untainted, the method of untainting
    std::vector<std::tuple<PhysRegIdPtr, uint8_t, uint8_t, DynInstPtr, UntaintMethod>> untaintQueue;

    const int queueLimit = cpu->idealUntaint ? std::numeric_limits<int>::max() : cpu->freeParam;

    int numIters = 0;
    int numUntainted = 0;

    do {
        numIters++;

        untaintQueue.clear();

        for (auto inst : instList[tid]) {
            if (inst->isSquashed())
                continue;

            bool destTainted = inst->isDestTainted();
            bool argsTainted = inst->isArgsTainted();
            BitVec& destTaintBcastMask = inst->destTaintBcastMask;
            BitVec& argsTaintBcastMask = inst->argsTaintBcastMask;

            // Clear the flags for registers that are already untainted

            for (int i = 0; i < inst->numDestRegs(); i++) {
                if (!inst->isDestIdxTainted(i)) {
                    destTaintBcastMask.at(i) = false;
                }
            }

            for (int i = 0; i < inst->numSrcRegs(); i++) {
                if (!inst->isArgsIdxTainted(i)) {
                    argsTaintBcastMask.at(i) = false;
                }
            }

            // Forward untaint propagation

            if (cpu->fwdUntaint) {
                if (!inst->isMemTransmit() && !argsTainted && destTainted) {
                    for (int i = 0; i < inst->numDestRegs(); i++) {
                        if (inst->isDestIdxTainted(i) && !destTaintBcastMask.at(i)) {
                            destTaintBcastMask.at(i) = true;
                        }
                    }
                }
            }

            // Backward untaint propogation
            if (cpu->bwdUntaint) {
                std::string instName = inst->staticInst->getName();
                std::vector<std::pair<RegId, PhysRegIdPtr>> bwdUntaintedRegs; // Used for logging purposes
                const char* opcodes = "add,addi,adc,adci,sub,subi,sbb,sbbi";

                if (strstr(opcodes, instName.c_str()) != nullptr)
                {
                    // If an ADD or SUB has an untainted dest and all but one src is untainted, then
                    // the remaining src can be untainted
                    // NOTE: This doesn't apply if the destination is the zero reg
                    if (!(inst->renamedDestIdx(0)->is(IntRegClass) &&
                          inst->renamedDestIdx(0)->index() == cpu->zeroRegIdx) &&
                        !inst->isNonCCDestTainted() &&
                        inst->numTaintedSrcRegs() == 1)
                    {
                        for (int i = 0; i < inst->numSrcRegs(); i++) {
                            if (inst->isArgsIdxTainted(i) && !argsTaintBcastMask.at(i)) {
                                argsTaintBcastMask.at(i) = true;
                                bwdUntaintedRegs.push_back({inst->srcRegIdx(i), inst->renamedSrcIdx(i)});
                                break;
                            }
                        }
                    }
                }
                else if (instName == "mov") {
                    // If a MOV has an untainted dest then src 2 can be untainted as well
                    if (!destTainted && inst->isArgsIdxTainted(1) && !argsTaintBcastMask.at(1)) {
                        bwdUntaintedRegs.push_back({inst->srcRegIdx(1), inst->renamedSrcIdx(1)});
                        argsTaintBcastMask.at(1) = 1;
                    }
                }
            }

            // Now that we've propagated the untaint, if the current instruction has any dest or src regs
            // that are waiting to be untainted, we add as many of them as we can to the queue

            assert(inst->numDestRegs() <= destTaintBcastMask.size());
            for (int i = 0; i < inst->numDestRegs(); i++) {
                if (destTaintBcastMask.at(i) && untaintQueue.size() < queueLimit) {
                    destTaintBcastMask.at(i) = false;
                    auto szAndOffs = inst->getDestRegSizeAndOffs(i);
                    untaintQueue.push_back(
                        std::make_tuple(inst->renamedDestIdx(i), szAndOffs.first, szAndOffs.second,
                                        inst, UntaintMethod::FwdUntaint));
                }
            }

            assert(inst->numSrcRegs() <= argsTaintBcastMask.size());
            for (int i = 0; i < inst->numSrcRegs(); i++) {
                if (argsTaintBcastMask.at(i) && untaintQueue.size() < queueLimit) {
                    argsTaintBcastMask.at(i) = false;
                    auto szAndOffs = inst->getSrcRegSizeAndOffs(i);
                    untaintQueue.push_back(
                        std::make_tuple(inst->renamedSrcIdx(i), szAndOffs.first, szAndOffs.second,
                                        inst, UntaintMethod::BwdUntaint));
                }
            }
        }

        for (auto t : untaintQueue) {
            auto physReg  = std::get<0>(t);
            auto size     = std::get<1>(t);
            auto offset   = std::get<2>(t);
            auto inst     = std::get<3>(t);
            auto utMethod = std::get<4>(t);

            if (cpu->readPartialTaint(physReg, size, offset)) {
                // [Rutvik, SPT] Stat collection stuff

                cpu->setUntaintMethod(physReg, utMethod);

                cpu->cpuStats.TotalUntaints++;
                if (utMethod == UntaintMethod::FwdUntaint) {
                    cpu->cpuStats.FwdUntaints++;
                }
                else if (utMethod == UntaintMethod::BwdUntaint) {
                    cpu->cpuStats.BwdUntaints++;
                }
            }

            cpu->setPartialTaint(physReg, false, size, offset);
        }

        numUntainted += untaintQueue.size();
    } while (cpu->idealUntaint && untaintQueue.size() > 0);
}

DynInstPtr
ROB::getResolvedPendingSquashInst(ThreadID tid)
{
    for (const DynInstPtr &inst : instList[tid]) {
        if (inst->hasPendingSquash()
            && inst->isUnsquashable()   // SPT: a delayed branch wait until it reaches VP
            && !inst->isSquashed()  // if it's already squashed, we ignore it
            ) {
            return inst;
        }
    }
    return nullptr;
}

} // namespace o3
} // namespace gem5
