/*
 * Copyright (c) 2026 The Regents of the University of California
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

#include "cpu/o3/doppelganger.hh"

#include "base/trace.hh"
#include "cpu/o3/dyn_inst.hh"
#include "cpu/o3/lsq_unit.hh"
#include "cpu/o3/thread_state.hh"
#include "debug/Doppelganger.hh"

namespace gem5
{

namespace o3
{

DoppelgangerRequest::DoppelgangerRequest(LSQUnit *lsq_unit,
                                         const DynInstPtr &_inst,
                                         Addr vaddr, unsigned size)
    : lsqUnit(lsq_unit),
      inst(_inst),
      predVaddr(vaddr),
      predSize(size),
      buffer(new uint8_t[size])
{
}

DoppelgangerRequest::~DoppelgangerRequest()
{
    delete pkt;
    delete[] buffer;
}

bool
DoppelgangerRequest::squashed() const
{
    return inst->isSquashed();
}

bool
DoppelgangerRequest::start()
{
    req = std::make_shared<Request>(
        predVaddr, predSize, Request::Flags(), inst->requestorId(),
        inst->pcState().instAddr(), inst->contextId());
    req->setReqInstSeqNum(inst->seqNum);

    inFlight = true;
    lsqUnit->getMMUPtr()->translateTiming(
        req, inst->thread->getTC(), this, BaseMMU::Read);

    // Translation may have finished inside that call, in which case finish()
    // has already run and may have given up.
    if (failed) {
        delete this;
        return false;
    }

    return true;
}

void
DoppelgangerRequest::finish(const Fault &fault, const RequestPtr &request,
                            gem5::ThreadContext *tc, BaseMMU::Mode mode)
{
    inFlight = false;

    // A fault here is not the load's fault -- we made this address up. Drop
    // the doppelganger and let the real load discover whatever is true.
    if (fault != NoFault || inst->isSquashed()) {
        DPRINTF(Doppelganger, "[sn:%llu] dropped: %s\n", inst->seqNum,
                fault != NoFault ? "translation faulted" : "squashed");
        failed = true;
        reapIfDone();
        return;
    }

    pkt = Packet::createRead(request);
    pkt->dataStatic(buffer);
    pkt->senderState = this;

    if (!lsqUnit->sendDoppelgangerPacket(pkt)) {
        // No free load port. Predicted accesses yield to real ones, so
        // rather than queue for a retry we simply give up on this one.
        DPRINTF(Doppelganger, "[sn:%llu] dropped: no cache port for %#x\n",
                inst->seqNum, predVaddr);
        failed = true;
        reapIfDone();
        return;
    }

    inFlight = true;
    DPRINTF(Doppelganger, "[sn:%llu] issued to %#x (%u bytes)\n",
            inst->seqNum, predVaddr, predSize);
}

void
DoppelgangerRequest::recvTimingResp(PacketPtr response)
{
    assert(response == pkt);
    inFlight = false;

    // A response that carries an invalidation means the line left us as the
    // data arrived; the bytes may already be out of date.
    if (response->isInvalidate())
        stale = true;

    complete = true;
    DPRINTF(Doppelganger, "[sn:%llu] data back for %#x%s\n", inst->seqNum,
            predVaddr, stale ? " (already stale)" : "");

    // If the load is sitting on this rather than making its own access, it
    // can have its data now.
    if (awaited && !abandoned && !inst->isSquashed()) {
        lsqUnit->completeAwaitedLoad(this);
        return;
    }

    reapIfDone();
}

void
DoppelgangerRequest::abandon()
{
    abandoned = true;
    reapIfDone();
}

void
DoppelgangerRequest::reapIfDone()
{
    if (abandoned && !inFlight)
        delete this;
}

} // namespace o3
} // namespace gem5
