#pragma once

#include <vector>
#include <cstdint>
#include <cstddef>

#include "base/bitfield.hh"

namespace gem5
{

namespace o3
{

// Rutvik, SPT
enum class UntaintMethod {
    NoUntaint,
    ReachedVP,
    FwdUntaint,
    BwdUntaint,
    ShadowL1,
    STLFwd,
    STLBwd,
};

typedef uint64_t BitVec;
static constexpr std::size_t BitVecBits = sizeof(BitVec) * 8;
typedef std::vector<BitVec> VecOfBitVec;
typedef std::vector<UntaintMethod> UntaintMethodTable;

}

}
