#pragma once

#include "cpu/inst_seq.hh"

namespace gem5
{

static inline constexpr InstSeqNum InvalidYRoT = -1;
static inline constexpr InstSeqNum NoYRoT = 0;

static inline bool
yrotValid(InstSeqNum yrot)
{
    return yrot != InvalidYRoT;
}

}
