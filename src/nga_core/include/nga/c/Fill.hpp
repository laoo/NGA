#pragma once

#include "nga/c/Ir.hpp"

namespace nga::c
{

/// Turns a loop that fills memory a byte at a time into one call of the
/// runtime's `__fill`: `for (i = from; i < bound; i = i + 1) a[i] = v;` over a
/// 16-bit counter of the Proc's own, with `a` and `v` the same at every turn,
/// is `__fill(a + i, bound - i, v)` and `i = bound` where the loop's test
/// first holds. Under `speed` only: the runtime's Proc is bytes a program did
/// not have, which `fit` does not take since it cannot give them back. See
/// docs/decisions/0265-a-loop-that-fills-calls-the-runtime.md.
void fillLoops( ir::Unit& unit, bool speed );

} // namespace nga::c
