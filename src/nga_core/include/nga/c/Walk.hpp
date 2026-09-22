#pragma once

#include "nga/c/Ir.hpp"

namespace nga::c
{

/// Walks a pointer beside a loop's 16-bit counter, where the loop reaches
/// elements through the counter: `flags[i]` with `i` stepped by one computed
/// `flags + i` into a pair of scratch bytes at every turn, six instructions of
/// 16-bit arithmetic; now a pointer of the Proc's own, `__pN`, is set to
/// `flags + i` where the loop is entered, stepped by one beside `i`, and the
/// element is read and written through it. The counter, and the test of it,
/// stay as they were. See
/// docs/decisions/0126-a-pointer-walks-beside-the-counter.md.
void walkPointers( ir::Unit& unit );

} // namespace nga::c
