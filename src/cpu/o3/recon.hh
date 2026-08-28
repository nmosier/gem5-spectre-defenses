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

#ifndef __CPU_O3_RECON_HH__
#define __CPU_O3_RECON_HH__

#include <map>
#include <vector>

#include "base/statistics.hh"
#include "base/types.hh"
#include "cpu/inst_seq.hh"
#include "cpu/o3/dyn_inst_ptr.hh"
#include "cpu/reg_class.hh"
#include "cpu/stt.hh"
#include "params/BaseO3CPU.hh"

namespace gem5
{

namespace o3
{

class CPU;

/**
 * ReCon: detection, management, and use of non-speculative information
 * leakage, after Aimoniotis et al., MICRO '23.
 *
 * A secure speculation scheme such as STT pays for protecting values that
 * an attacker can already read off the memory hierarchy, because the
 * program itself already sent them there as addresses. ReCon finds those
 * values cheaply and hands the underlying scheme permission to stop
 * protecting them.
 *
 * The cheap part is limiting the search to *direct dependence load pairs*:
 * a load whose result is used, with no intervening producer, as the address
 * of a second load. When the second load commits, the value the first one
 * read has provably been sent to the memory hierarchy as an address by
 * non-speculative execution, so it is public. ReCon marks the address the
 * first load read from as *revealed* (see Recon::commitInst), and the cache
 * hierarchy remembers that per word (see mem/recon.hh). A later load of a
 * revealed word learns so from its cache response and is not treated as a
 * root of taint at all (see Recon::loadRevealed).
 *
 * A store to a revealed word conceals it again: its new value has not been
 * leaked. That happens in the caches, on the store's path to memory.
 */
class Recon
{
  public:
    Recon(CPU *_cpu, const BaseO3CPUParams &params);

    bool enabled() const { return reconEnabled; }

    /**
     * Update the load-pair table for a committing instruction, and reveal
     * the address of the older load if this one completes a pair. Must be
     * called exactly once per committed instruction, in program order.
     */
    void commitInst(const DynInstPtr &inst);

    /**
     * Record that a load read a word the memory hierarchy reports as
     * already revealed, and so is not itself a root of taint.
     */
    void loadRevealed(const DynInstPtr &inst);

    /** Discard untaint records for instructions that no longer exist. */
    void squash(InstSeqNum squashed_seq_num);

    /**
     * Resolve a youngest-root-of-taint through any ReCon-untainted loads.
     *
     * STT names a taint by the sequence number of the load that produced
     * it, which lets a single untaint broadcast retire every taint older
     * than a given instruction at once. A ReCon untaint does not fit that
     * order: it retires one specific load's taint while older ones stand.
     * So instead of rewriting the root recorded in every dependent
     * instruction, a revealed load leaves behind the root that its own
     * address operands carried, and taint queries follow that chain here.
     *
     * This is only sound while the root being replaced is the *only* one
     * the value carries -- STT's largest-root summary hides the rest, and
     * retiring the largest would take them with it. So the walk stops as
     * soon as `unique` is false, leaving the largest root standing, which
     * is what plain STT would have tested anyway. Each step is strictly
     * older than the last, so the walk terminates, and it is skipped
     * entirely for a value that is not tainted.
     */
    InstSeqNum resolveYRoT(InstSeqNum yrot, bool unique,
                           InstSeqNum untaint_broadcast);

    /**
     * Forget every load-pair table entry that a committing store overwrites.
     *
     * A tracked entry says "an earlier load read a value from this address".
     * Once a store lands there that is no longer true, and revealing the
     * address on the strength of the stale entry would mark a value the
     * program never used as an address.
     */
    void concealStore(const DynInstPtr &inst);

  private:
    /**
     * One load-pair table entry. The tag is what makes a table smaller
     * than the physical register file safe: two registers that share an
     * entry simply miss each other, costing an optimization rather than
     * correctness.
     */
    struct LPTEntry
    {
        bool valid = false;
        RegIndex tag = 0;
        Addr addr = 0;
        bool secure = false;
    };

    LPTEntry &entryFor(RegIndex reg) { return lpt[reg % lpt.size()]; }
    const LPTEntry &
    entryFor(RegIndex reg) const
    {
        return lpt[reg % lpt.size()];
    }

    /** Whether a physical register can index the load-pair table. */
    static bool trackable(PhysRegIdPtr reg);

    void record(PhysRegIdPtr reg, Addr addr, bool secure);
    bool lookup(PhysRegIdPtr reg, Addr &addr, bool &secure) const;
    void invalidate(PhysRegIdPtr reg);

    CPU *cpu;

    const bool reconEnabled;

    /** The load-pair table, indexed by physical integer register. */
    std::vector<LPTEntry> lpt;

    /** What a revealed load's root of taint falls back to. */
    struct FallbackRoot
    {
        InstSeqNum yrot = NoYRoT;
        bool unique = true;
    };

    /**
     * For each in-flight load that ReCon untainted, the root of taint that
     * still applies to its result. Ordered so that a squash can drop the
     * younger half in one range erase.
     */
    std::map<InstSeqNum, FallbackRoot> revealedRoots;

    struct ReconStats : public statistics::Group
    {
        ReconStats(statistics::Group *parent);

        /** Loads whose address ReCon remembered for a possible pair. */
        statistics::Scalar trackedLoads;
        /** Committed loads that completed a direct-dependence load pair. */
        statistics::Scalar loadPairs;
        /** Reveal messages sent to the L1. */
        statistics::Scalar reveals;
        /** Reveal messages dropped because the cache port was busy. */
        statistics::Scalar revealsDropped;
        /** Loads whose address came from more than one register. */
        statistics::Scalar ambiguousAddresses;
        /** Tracked loads dropped because a store overwrote their word. */
        statistics::Scalar entriesOverwritten;
        /** Loads that read a word the hierarchy reported as revealed. */
        statistics::Scalar revealedLoads;
        /** Steps taken walking chains of revealed loads in taint queries. */
        statistics::Scalar untaintSteps;
    } stats;
};

} // namespace o3
} // namespace gem5

#endif // __CPU_O3_RECON_HH__
