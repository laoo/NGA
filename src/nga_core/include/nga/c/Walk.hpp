#pragma once

#include "nga/c/Ir.hpp"
#include "nga/c/Loops.hpp"

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
///
/// An address that adds to its base the counter plus something the loop does
/// not change, `t[k + row]`, walks from `t + row + k` the same way; and what
/// the loop does not change is what `effects` says the functions it calls do
/// not write — see
/// docs/decisions/0305-a-pointer-walks-from-an-offset-beside-the-counter.md.
void walkPointers( ir::Unit& unit, Effects const& effects );

} // namespace nga::c
