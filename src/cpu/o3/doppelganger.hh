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

#ifndef __CPU_O3_DOPPELGANGER_HH__
#define __CPU_O3_DOPPELGANGER_HH__

#include <cstdint>

#include "arch/generic/mmu.hh"
#include "base/types.hh"
#include "cpu/o3/dyn_inst_ptr.hh"
#include "mem/packet.hh"
#include "mem/request.hh"

namespace gem5
{

namespace o3
{

class LSQUnit;

/**
 * The memory access a Doppelganger Load makes on its predicted address
 * (Kvalsvik et al., ISCA '23).
 *
 * A secure speculation scheme holds a load back because its address depends
 * on a value that may be a secret. The address a stride predictor produces
 * from committed history does not, so that access may be made straight
 * away. This class is that access: it translates the predicted address,
 * fetches the line, and keeps the bytes until the real load catches up. If
 * the real address turns out to match, the load is satisfied from here and
 * never touches the cache; the memory latency was spent during the delay
 * instead of after it. If it does not match, this is thrown away and the
 * load runs exactly as it would have.
 *
 * It is deliberately *not* an LSQRequest. That class assumes one request per
 * instruction -- its destructor clears the instruction's `savedRequest`, and
 * installing it overwrites the load queue entry's request pointer -- so a
 * second, concurrent one would quietly corrupt the real load's bookkeeping.
 *
 * Everything about it is best-effort. A doppelganger that cannot translate,
 * cannot find a free cache port, or straddles a cache line is simply
 * dropped: the load then runs unaided, which is slower and never wrong.
 */
class DoppelgangerRequest : public BaseMMU::Translation,
                            public Packet::SenderState
{
  public:
    DoppelgangerRequest(LSQUnit *lsq_unit, const DynInstPtr &inst,
                        Addr vaddr, unsigned size);
    ~DoppelgangerRequest();

    /**
     * Translate the predicted address and, if a load port is free, send the
     * access. Returns false if the doppelganger could not be started, in
     * which case it has already destroyed itself.
     */
    bool start();

    /** BaseMMU::Translation. */
    void markDelayed() override {}
    void finish(const Fault &fault, const RequestPtr &req,
                gem5::ThreadContext *tc, BaseMMU::Mode mode) override;
    bool squashed() const override;

    /** Take the response to our access. */
    void recvTimingResp(PacketPtr pkt);

    /** Whether the fetched bytes are here and still describe memory. */
    bool ready() const { return complete && !stale; }

    /** Whether this doppelganger can no longer produce anything useful. */
    bool dead() const { return failed || stale; }

    /** Whether the access has been sent and is still on its way back. */
    bool pending() const { return !complete && !failed && inFlight; }

    /**
     * The load has caught up, found this doppelganger fetching exactly what
     * it wants, and is now waiting on it rather than making the access
     * again. This is the case the whole scheme is built for: the memory
     * latency was started during the delay, so what is left of it is all the
     * load still has to pay.
     */
    void await() { awaited = true; }
    bool isAwaited() const { return awaited; }

    const DynInstPtr &instruction() const { return inst; }
    Addr vaddr() const { return predVaddr; }
    unsigned size() const { return predSize; }
    const uint8_t *data() const { return buffer; }

    /** Whether this access covers any byte of [begin, end). */
    bool overlaps(Addr begin, Addr end) const
    {
        return predVaddr < end && begin < predVaddr + predSize;
    }

    /**
     * The bytes no longer reflect memory -- something wrote them, or the
     * line was taken away -- so the load must fetch them itself.
     */
    void markStale() { stale = true; }

    /**
     * Nobody wants this any more. Frees it now, or as soon as the access
     * still in flight comes back.
     */
    void abandon();

  private:
    /** Destroy if abandoned and nothing is left outstanding. */
    void reapIfDone();

    LSQUnit *lsqUnit;
    DynInstPtr inst;

    const Addr predVaddr;
    const unsigned predSize;

    RequestPtr req;
    PacketPtr pkt = nullptr;
    uint8_t *buffer;

    /** A translation or a packet is still on its way back to us. */
    bool inFlight = false;
    bool complete = false;
    bool failed = false;
    bool stale = false;
    bool abandoned = false;
    bool awaited = false;
};

} // namespace o3
} // namespace gem5

#endif // __CPU_O3_DOPPELGANGER_HH__
