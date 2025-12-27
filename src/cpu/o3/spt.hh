#pragma once

#include <vector>

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

typedef std::vector<bool> BitVec;
typedef std::vector<BitVec> VecOfBitVec;
typedef std::vector<UntaintMethod> UntaintMethodTable;

}

}
