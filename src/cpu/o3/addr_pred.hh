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

#ifndef __CPU_O3_ADDR_PRED_HH__
#define __CPU_O3_ADDR_PRED_HH__

#include <deque>
#include <vector>

#include "base/sat_counter.hh"
#include "base/statistics.hh"
#include "base/types.hh"
#include "cpu/inst_seq.hh"
#include "params/BaseO3CPU.hh"

namespace gem5
{

namespace o3
{

/**
 * The key a load is tracked under.
 *
 * Every microop of a macroop reports the same instruction address, so on a
 * CISC ISA two loads within one macroop would otherwise share a history and
 * train each other into nonsense. Folding in the micro-PC separates them.
 * A collision here costs accuracy and nothing else: a prediction is always
 * checked against the real address before its data is used.
 */
inline Addr
addrPredKey(Addr inst_addr, MicroPC upc)
{
    return (inst_addr << 8) ^ upc;
}

/**
 * A PC-indexed stride address predictor, as used by Doppelganger Loads
 * (Kvalsvik et al., ISCA '23).
 *
 * The security of the whole scheme rests on one property of this class:
 * **it is only ever trained by loads that commit**. A prediction is
 * therefore a function of architectural state alone, and an access made to
 * a predicted address leaks nothing about a speculatively loaded value --
 * which is exactly what lets a secure speculation scheme allow it while
 * still refusing the real, secret-dependent address. Nothing here may take
 * an address from a load that has not committed.
 *
 * The table is deliberately simple: the paper's point is that an ordinary
 * stride prefetcher, which processors already have, is enough to recover a
 * useful share of the memory parallelism these schemes give up. Entries are
 * tagged with the full PC so that aliasing cannot silently substitute one
 * load's history for another's.
 *
 * Predictions run ahead of commits. Several instances of the same load are
 * usually in flight at once, so each prediction advances a running address
 * (`specAddr`) rather than re-reading the last committed one; otherwise
 * every instance in a loop would be handed the same address. That running
 * address is speculative *position*, never speculative *data*: it is the
 * last committed address plus a whole number of committed strides. A squash
 * throws it away and the next prediction restarts from the last commit.
 */
class AddressPredictor
{
  public:
    AddressPredictor(statistics::Group *parent,
                     const BaseO3CPUParams &params);

    bool enabled() const { return predEnabled; }

    /**
     * Predict the address and access size of the next dynamic instance of a
     * load. Called in program order, once per decoded load. Returns false if
     * this load has no confident history, in which case it simply runs
     * unpredicted.
     *
     * The size is predicted too because a doppelganger has to issue before
     * its load executes, and until then the core does not know how wide the
     * access will be. A wrong size is caught by the same check that catches
     * a wrong address.
     */
    bool predict(Addr pc, InstSeqNum seq_num, Addr &addr, unsigned &size);

    /**
     * Train on a load that has committed, and only on one that has
     * committed. `addr` and `size` describe its access.
     */
    void update(Addr pc, InstSeqNum seq_num, Addr addr, unsigned size);

    /**
     * Undo the predictions made for instructions younger than
     * `squashed_seq_num`, putting each entry's running address back exactly
     * where it stood before them.
     *
     * Rewinding rather than restarting matters more than it looks. Restarting
     * from the last committed address strands the running address behind by
     * however many instances were in flight -- a dozen, in a loop waiting on
     * memory -- and since every later prediction advances by one stride from
     * there, the gap never closes and every prediction thereafter is wrong.
     */
    void squash(InstSeqNum squashed_seq_num);

  private:
    struct Entry
    {
        bool valid = false;
        Addr tag = 0;

        /** Whether a committed instance has set the baseline address. */
        bool trained = false;

        /** Address and stride of the most recent *committed* instance. */
        Addr lastAddr = 0;
        int64_t stride = 0;
        unsigned size = 0;
        SatCounter8 confidence;

        /**
         * How many instances of this load have been looked up but have not
         * yet committed.
         *
         * A prediction is `lastAddr + (outstanding + 1) * stride` rather
         * than a running total, so it stays anchored to committed state:
         * several instances of a load are normally in flight at once and
         * each needs a different address, but every prediction is still
         * measured from the last address that actually committed. A running
         * total would drift -- one stride behind for every instance whose
         * lookup was skipped for want of confidence -- and never recover.
         */
        unsigned outstandingPreds = 0;

        /** For least-recently-used replacement within a set. */
        uint64_t lastUsed = 0;

        explicit Entry(const SatCounter8 &init) : confidence(init) {}
    };

    Entry *findEntry(Addr pc);
    Entry &allocateEntry(Addr pc);
    unsigned
    setOf(Addr pc) const
    {
        return (pc ^ (pc >> 10)) % numSets;
    }

    const bool predEnabled;
    const unsigned numSets;
    const unsigned assoc;
    const unsigned confThreshold;

    std::vector<Entry> table;

    /**
     * One record per prediction that has not yet been committed or undone,
     * oldest first, holding enough to put the entry back as it was.
     */
    struct Outstanding
    {
        InstSeqNum seqNum;
        unsigned entry;
    };
    std::deque<Outstanding> outstanding;

    /** Monotonic tick for LRU ordering. */
    uint64_t useCounter = 0;

    struct AddrPredStats : public statistics::Group
    {
        AddrPredStats(statistics::Group *parent);

        /** Predictions requested for a decoded load. */
        statistics::Scalar lookups;
        /** Predictions actually produced. */
        statistics::Scalar predictions;
        /** Committed loads that trained the table. */
        statistics::Scalar updates;
        /** Trainings that confirmed the entry's stride. */
        statistics::Scalar strideHits;
        /** Trainings that changed it. */
        statistics::Scalar strideChanges;
        /** Entries evicted from a full set. */
        statistics::Scalar evictions;
        /** Predictions undone by a squash. */
        statistics::Scalar rewound;
    } stats;
};

} // namespace o3
} // namespace gem5

#endif // __CPU_O3_ADDR_PRED_HH__
