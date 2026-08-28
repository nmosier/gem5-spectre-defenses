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

#ifndef __MEM_RECON_HH__
#define __MEM_RECON_HH__

#include <algorithm>
#include <cstdint>

#include "base/types.hh"

namespace gem5
{

/**
 * ReCon tracks, for every cache line, which of its words hold a value that
 * non-speculative execution has already leaked -- a value that was loaded
 * and then dereferenced by a second, directly dependent load. Such a value
 * is public, so a secure speculation scheme need not protect later loads of
 * it.
 *
 * Reveal state is kept at the granularity of aligned 8-byte words, which
 * bounds the cost to one byte per 64-byte line. A pointer is exactly one
 * such word, which is what makes the granularity a good fit; conversely, a
 * narrower load can never reveal, because doing so would mark bytes that
 * were never used as an address.
 */
namespace recon
{

/** Granularity of reveal tracking. */
inline constexpr unsigned WordSize = 8;

/** Words trackable per line, bounded by the width of the bit-vector. */
inline constexpr unsigned MaxWords = 64;

/**
 * Whether an access covers exactly one aligned word, the only shape whose
 * value a load pair is allowed to reveal. Revealing a wider or narrower
 * access would mark bytes that never reached the memory hierarchy as an
 * address.
 */
inline bool
isWordAccess(Addr addr, unsigned size)
{
    return size == WordSize && (addr % WordSize) == 0;
}

/**
 * Mask of the words that [addr, addr + size) touches, relative to the start
 * of the line containing addr. Words past the width of the bit-vector are
 * left out, which leaves them permanently concealed.
 */
inline uint64_t
wordMask(Addr addr, unsigned size, unsigned blk_size)
{
    if (size == 0)
        return 0;

    const Addr offset = addr % blk_size;
    const unsigned first = offset / WordSize;
    if (first >= MaxWords)
        return 0;

    const unsigned last =
        std::min<unsigned>((offset + size - 1) / WordSize, MaxWords - 1);
    const unsigned num_words = last - first + 1;
    const uint64_t low_bits =
        (num_words >= MaxWords) ? ~0ULL : ((1ULL << num_words) - 1);

    return low_bits << first;
}

} // namespace recon
} // namespace gem5

#endif // __MEM_RECON_HH__
