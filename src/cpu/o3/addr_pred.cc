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

#include "cpu/o3/addr_pred.hh"

#include <algorithm>

#include "base/trace.hh"
#include "base/logging.hh"
#include "debug/AddrPred.hh"

namespace gem5
{

namespace o3
{

AddressPredictor::AddressPredictor(statistics::Group *parent,
                                   const BaseO3CPUParams &params)
    : predEnabled(params.doppelgangerLoads),
      numSets(params.addrPredEntries /
              std::max<unsigned>(params.addrPredAssoc, 1)),
      assoc(std::max<unsigned>(params.addrPredAssoc, 1)),
      confThreshold(params.addrPredConfThreshold),
      stats(parent)
{
    if (!predEnabled)
        return;

    fatal_if(numSets == 0,
             "Address predictor needs at least one set: %u entries over %u "
             "ways leaves none", params.addrPredEntries, assoc);

    table.assign((size_t)numSets * assoc,
                 Entry(SatCounter8(params.addrPredConfBits)));
}

AddressPredictor::Entry *
AddressPredictor::findEntry(Addr pc)
{
    const unsigned set = setOf(pc);
    for (unsigned way = 0; way < assoc; way++) {
        Entry &entry = table[(size_t)set * assoc + way];
        // A full-PC tag: aliasing would let one load's history stand in for
        // another's, and a prediction is only as safe as it is attributable.
        if (entry.valid && entry.tag == pc)
            return &entry;
    }
    return nullptr;
}

AddressPredictor::Entry &
AddressPredictor::allocateEntry(Addr pc)
{
    const unsigned set = setOf(pc);
    Entry *victim = &table[(size_t)set * assoc];

    for (unsigned way = 0; way < assoc; way++) {
        Entry &entry = table[(size_t)set * assoc + way];
        if (!entry.valid) {
            victim = &entry;
            break;
        }
        if (entry.lastUsed < victim->lastUsed)
            victim = &entry;
    }

    if (victim->valid)
        stats.evictions++;

    victim->valid = true;
    victim->tag = pc;
    victim->trained = false;
    victim->stride = 0;
    victim->confidence.reset();
    victim->outstandingPreds = 0;
    return *victim;
}

bool
AddressPredictor::predict(Addr pc, InstSeqNum seq_num, Addr &addr,
                          unsigned &size)
{
    if (!predEnabled)
        return false;

    stats.lookups++;

    Entry *entry = findEntry(pc);
    if (!entry) {
        // Allocate on first sight rather than waiting for a commit. The
        // in-flight count below only means anything if every instance of a
        // load is counted on the way in and again on the way out; a load
        // first seen here but first counted at commit would leave the count
        // permanently low, and every prediction short by that many strides.
        entry = &allocateEntry(pc);
    }

    entry->lastUsed = ++useCounter;

    // Count this instance whether or not we go on to predict for it. The
    // count is how far ahead of the last commit we are, so it has to follow
    // instances, not predictions.
    entry->outstandingPreds++;
    outstanding.push_back({seq_num, (unsigned)(entry - table.data())});

    // An unconfident entry has not shown a stride often enough to be worth
    // acting on. Predicting anyway would spend a memory access to learn
    // nothing.
    if (entry->confidence < confThreshold)
        return false;

    addr = entry->lastAddr + (int64_t)entry->outstandingPreds * entry->stride;
    size = entry->size;

    stats.predictions++;
    DPRINTF(AddrPred, "pc %#x: predicting %#x (last committed %#x, stride "
            "%+lld, %u in flight)\n", pc, addr, entry->lastAddr,
            entry->stride, entry->outstandingPreds);

    return true;
}

void
AddressPredictor::update(Addr pc, InstSeqNum seq_num, Addr addr,
                         unsigned size)
{
    if (!predEnabled)
        return;

    stats.updates++;

    // Everything this old is resolved and can never need rewinding.
    while (!outstanding.empty() && outstanding.front().seqNum <= seq_num)
        outstanding.pop_front();

    Entry *entry = findEntry(pc);
    if (!entry) {
        // Only reachable if the entry was evicted between this load's
        // lookup and its commit.
        Entry &fresh = allocateEntry(pc);
        fresh.lastAddr = addr;
        fresh.size = size;
        fresh.lastUsed = ++useCounter;
        return;
    }

    entry->lastUsed = ++useCounter;

    // This instance has resolved, so it is no longer ahead of the commit
    // point.
    if (entry->outstandingPreds > 0)
        entry->outstandingPreds--;

    if (!entry->trained) {
        // First committed instance: nothing to compare against yet.
        entry->trained = true;
        entry->lastAddr = addr;
        entry->size = size;
        return;
    }

    const int64_t stride = (int64_t)(addr - entry->lastAddr);
    if (stride == entry->stride && size == entry->size) {
        entry->confidence++;
        stats.strideHits++;
    } else {
        // A new stride is worth learning, but not worth trusting yet.
        entry->stride = stride;
        entry->size = size;
        entry->confidence.reset();
        stats.strideChanges++;
    }

    entry->lastAddr = addr;

    DPRINTF(AddrPred, "pc %#x: committed %#x, stride %+lld, confidence %u\n",
            pc, addr, entry->stride, (unsigned)entry->confidence);
}

void
AddressPredictor::squash(InstSeqNum squashed_seq_num)
{
    if (!predEnabled)
        return;

    // Undo the youngest predictions first, so each entry is restored through
    // exactly the states it passed through.
    while (!outstanding.empty() &&
           outstanding.back().seqNum > squashed_seq_num) {
        Entry &entry = table[outstanding.back().entry];
        if (entry.outstandingPreds > 0)
            entry.outstandingPreds--;
        outstanding.pop_back();
        stats.rewound++;
    }
}

AddressPredictor::AddrPredStats::AddrPredStats(statistics::Group *parent)
    : statistics::Group(parent, "addrPred"),
      ADD_STAT(lookups, statistics::units::Count::get(),
               "Number of decoded loads the predictor was consulted for"),
      ADD_STAT(predictions, statistics::units::Count::get(),
               "Number of addresses predicted"),
      ADD_STAT(updates, statistics::units::Count::get(),
               "Number of committed loads that trained the predictor"),
      ADD_STAT(strideHits, statistics::units::Count::get(),
               "Number of trainings that confirmed the entry's stride"),
      ADD_STAT(strideChanges, statistics::units::Count::get(),
               "Number of trainings that changed the entry's stride"),
      ADD_STAT(evictions, statistics::units::Count::get(),
               "Number of predictor entries evicted from a full set"),
      ADD_STAT(rewound, statistics::units::Count::get(),
               "Number of predictions undone by a squash")
{
}

} // namespace o3
} // namespace gem5
