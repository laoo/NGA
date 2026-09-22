#pragma once

#include "nga/c/Ir.hpp"

namespace nga::c

{

/// Sends a call of the runtime to the Proc written for what the operands turn
/// out to be, and hands that Proc its operands the way round it is cheapest
/// in: a multiplication of two values the ranges prove to be bytes goes to
/// `__mul8to16`, whose round is a byte's where `__mul16`'s is a pair's and
/// which needs no zero high bytes written into its arguments
/// (docs/decisions/0122-a-multiply-of-two-bytes.md); and either Proc walks
/// `left` until nothing of it is left, so the operand the ranges hold smaller
/// goes there (docs/decisions/0144-a-multiply-stops-when-its-multiplier-does.md,
/// docs/decisions/0170-a-wide-multiply-stops-when-its-multiplier-does.md).
/// The first reader of docs/decisions/0121-what-a-value-can-hold.md.
void narrowRuntimeCalls( ir::Unit& unit );

/// Reaches an element through `X` where the index can only hold a byte,
/// whatever its type: `t[i & 255]` with `i` a `u16` was the address `t + (i &
/// 255)` computed into a pair and the element read through it, and is `lda
/// t,x` now, since the ranges say the index fits. An array of 16-bit elements
/// takes an index of at most 127, the doubled one having to fit `X` too. See
/// docs/decisions/0127-an-index-that-fits-a-byte-goes-through-x.md.
void narrowIndexes( ir::Unit& unit );

} // namespace nga::c
