#pragma once

#include "nga/c/Ir.hpp"

namespace nga::c
{

/// Counts a loop that only counts up to its wrap instead: `for (k = 0; k < b;
/// k = k + 1)` whose counter nothing but its own test and step reads, and
/// nothing reads after the loop, runs `c` from `0 - b` up to zero, so that its
/// test is `c != 0` — `inx / bne` for a byte, and no compare with the bound at
/// every turn. See docs/decisions/0274-a-loop-that-only-counts-runs-to-its-wrap.md.
void countToWrap( ir::Unit& unit );

} // namespace nga::c
