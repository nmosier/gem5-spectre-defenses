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

#include "cpu/o3/recon.hh"

#include <algorithm>

#include "base/logging.hh"
#include "cpu/o3/cpu.hh"
#include "cpu/o3/dyn_inst.hh"
#include "debug/Recon.hh"
#include "mem/recon.hh"
#include "mem/request.hh"

namespace gem5
{

namespace o3
{

Recon::Recon(CPU *_cpu, const BaseO3CPUParams &params)
    : cpu(_cpu),
      reconEnabled(params.recon),
      stats(_cpu)
{
    if (!reconEnabled)
        return;

    // A table with one entry per physical integer register never conflicts.
    // Smaller tables are the sensitivity study of the paper's Section 6.6:
    // load pairs sit close together, so conflicts cost little.
    const unsigned entries = params.reconLPTEntries ?
        params.reconLPTEntries : params.numPhysIntRegs;
    fatal_if(entries == 0, "ReCon load-pair table must have entries");
    lpt.resize(entries);
}

bool
Recon::trackable(PhysRegIdPtr reg)
{
    // Only integer registers hold addresses, and a register that is not
    // really renamed does not identify a single dynamic value.
    return reg->is(IntRegClass) && !reg->isFixedMapping();
}

void
Recon::record(PhysRegIdPtr reg, Addr addr, bool secure)
{
    if (!trackable(reg))
        return;

    LPTEntry &entry = entryFor(reg->index());
    entry.valid = true;
    entry.tag = reg->index();
    entry.addr = addr;
    entry.secure = secure;
}

bool
Recon::lookup(PhysRegIdPtr reg, Addr &addr, bool &secure) const
{
    if (!trackable(reg))
        return false;

    const LPTEntry &entry = entryFor(reg->index());
    if (!entry.valid || entry.tag != reg->index())
        return false;

    addr = entry.addr;
    secure = entry.secure;
    return true;
}

void
Recon::invalidate(PhysRegIdPtr reg)
{
    if (!trackable(reg))
        return;

    LPTEntry &entry = entryFor(reg->index());
    // Leave a conflicting register's entry alone: it is not ours to clear,
    // and the tag check already keeps us from reading it.
    if (entry.tag == reg->index())
        entry.valid = false;
}

void
Recon::commitInst(const DynInstPtr &inst)
{
    if (!reconEnabled)
        return;

    // Once a load commits it is older than the untaint broadcast, so
    // resolveYRoT stops before ever reaching its record.
    revealedRoots.erase(inst->seqNum);

    // Whatever the destination registers used to stand for, they hold this
    // instruction's result now. Only a load re-arms them, just below.
    for (int i = 0; i < inst->numDestRegs(); i++)
        invalidate(inst->renamedDestIdx(i));

    if (!inst->effAddrValid())
        return;

    // HTM and TLBI commands are modelled as eight-byte loads of address
    // zero. They read no memory, so they neither reveal nor are worth
    // tracking; without this they would record physical word zero.
    if (inst->isHtmCmd() || inst->effAddr == 0)
        return;

    // A store makes any entry naming the words it writes stale: the value
    // an earlier load read is no longer what lives there.
    if (inst->isStore() || inst->isAtomic())
        concealStore(inst);

    // Atomics both read and write their address, so their result is never
    // a plain loaded value; leave them out of pair detection.
    if (!inst->isLoad() || inst->isAtomic())
        return;

    const bool secure = inst->memReqFlags & Request::SECURE;

    // Find the register this load dereferences. An address built from more
    // than one register tells us less than it appears to: what reached the
    // memory hierarchy is the combination, so no single register's value
    // follows from it, and revealing on one register's account would mark a
    // value the program never exposed. Insisting on a single register keeps
    // us to the paper's direct dependence -- a loaded value used as the
    // address, give or take an immediate.
    PhysRegIdPtr addr_reg = nullptr;
    for (int i = 0; i < inst->numSrcRegs(); i++) {
        if (!inst->staticInst->srcTransmitted(i))
            continue;

        PhysRegIdPtr src = inst->renamedSrcIdx(i);
        if (!trackable(src))
            continue;

        if (addr_reg) {
            addr_reg = nullptr; // more than one: we cannot attribute it
            stats.ambiguousAddresses++;
            break;
        }
        addr_reg = src;
    }

    // Does this load dereference a value an earlier load produced? If so
    // the two form a direct-dependence pair, and the word the earlier load
    // read has now been handed to the memory hierarchy as an address by
    // non-speculative execution. It is public.
    Addr revealed_addr;
    bool revealed_secure;
    if (addr_reg && lookup(addr_reg, revealed_addr, revealed_secure)) {
        stats.loadPairs++;
        DPRINTF(Recon, "[sn:%llu] load pair: %#x revealed by load of %#x\n",
                inst->seqNum, revealed_addr, inst->physEffAddr);

        if (cpu->reconReveal(revealed_addr, revealed_secure))
            stats.reveals++;
        else
            stats.revealsDropped++;
    }

    // Remember where this load read from, in case a later load
    // dereferences its result. Only an aligned word can be revealed: a
    // narrower load would mark bytes that never became an address. A value
    // that is already public needs no second reveal.
    if (inst->reconRevealed() ||
        !recon::isWordAccess(inst->physEffAddr, inst->effSize))
        return;

    // Only a register that holds the loaded value may be tracked. Some
    // loads also write an updated base address (ARM's writeback forms, for
    // instance), and that register holds address arithmetic, not data read
    // from memory. Dereferencing it later would say nothing about the value
    // at this load's address, so revealing that address on its account
    // would expose a word the program never used as an address. Rather than
    // ask each ISA which destination is the data, track only the
    // unambiguous case of a single integer destination.
    PhysRegIdPtr data_reg = nullptr;
    for (int i = 0; i < inst->numDestRegs(); i++) {
        PhysRegIdPtr dest = inst->renamedDestIdx(i);
        if (!trackable(dest))
            continue;
        if (data_reg)
            return; // more than one candidate: we cannot tell them apart
        data_reg = dest;
    }

    if (data_reg) {
        record(data_reg, inst->physEffAddr, secure);
        stats.trackedLoads++;
    }
}

void
Recon::loadRevealed(const DynInstPtr &inst)
{
    if (!reconEnabled)
        return;

    assert(inst->isLoad());
    assert(yrotValid(inst->yrotSrcs));
    // Every root a load can inherit belongs to an older instruction, which
    // is what makes the chain walked by resolveYRoT finite.
    assert(inst->yrotSrcs < inst->seqNum);

    revealedRoots[inst->seqNum] = {inst->yrotSrcs, inst->yrotSrcsUnique};
    stats.revealedLoads++;

    DPRINTF(Recon, "[sn:%llu] load of %#x read a revealed word; root of "
            "taint falls back to [sn:%llu]%s\n",
            inst->seqNum, inst->physEffAddr, inst->yrotSrcs,
            inst->yrotSrcsUnique ? "" : " (not the only one)");
}

void
Recon::concealStore(const DynInstPtr &inst)
{
    // The store's own address range, and the words it overlaps.
    const Addr store_begin = inst->physEffAddr;
    const Addr store_end = store_begin + inst->effSize;

    // Small enough to walk: the table has one entry per physical integer
    // register, and only committing stores pay for it.
    for (LPTEntry &entry : lpt) {
        if (!entry.valid)
            continue;

        const Addr word_end = entry.addr + recon::WordSize;
        if (entry.addr < store_end && store_begin < word_end) {
            DPRINTF(Recon, "[sn:%llu] store to %#x drops the tracked load "
                    "of %#x\n", inst->seqNum, store_begin, entry.addr);
            entry.valid = false;
            stats.entriesOverwritten++;
        }
    }
}

void
Recon::squash(InstSeqNum squashed_seq_num)
{
    if (!reconEnabled || revealedRoots.empty())
        return;

    revealedRoots.erase(revealedRoots.upper_bound(squashed_seq_num),
                        revealedRoots.end());
}

InstSeqNum
Recon::resolveYRoT(InstSeqNum yrot, bool unique, InstSeqNum untaint_broadcast)
{
    // Stop at the first root we cannot prove is the only one: retiring it
    // would retire the roots it stands in front of along with it.
    while (unique && yrot > untaint_broadcast) {
        const auto it = revealedRoots.find(yrot);
        if (it == revealedRoots.end())
            break;

        assert(it->second.yrot < yrot);
        yrot = it->second.yrot;
        unique = it->second.unique;
        stats.untaintSteps++;
    }

    return yrot;
}

Recon::ReconStats::ReconStats(statistics::Group *parent)
    : statistics::Group(parent, "recon"),
      ADD_STAT(trackedLoads, statistics::units::Count::get(),
               "Number of committed loads recorded in the load-pair table"),
      ADD_STAT(loadPairs, statistics::units::Count::get(),
               "Number of direct-dependence load pairs detected at commit"),
      ADD_STAT(reveals, statistics::units::Count::get(),
               "Number of reveal messages sent to the L1"),
      ADD_STAT(revealsDropped, statistics::units::Count::get(),
               "Number of reveal messages dropped, cache port unavailable"),
      ADD_STAT(ambiguousAddresses, statistics::units::Count::get(),
               "Number of committed loads whose address came from more than "
               "one register, so no single one could be credited"),
      ADD_STAT(entriesOverwritten, statistics::units::Count::get(),
               "Number of tracked loads dropped by a store to their word"),
      ADD_STAT(revealedLoads, statistics::units::Count::get(),
               "Number of loads that read an already-revealed word"),
      ADD_STAT(untaintSteps, statistics::units::Count::get(),
               "Number of roots of taint retired by walking a revealed load")
{
}

} // namespace o3
} // namespace gem5
