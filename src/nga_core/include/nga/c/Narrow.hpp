#pragma once

#include "nga/c/Ir.hpp"

namespace nga::c
{

/// Narrows what the subset's own rules widened: a 16-bit value whose high byte
/// is known to be zero is computed in one byte, and a widening nothing needs is
/// dropped — the first step of the order
/// docs/decisions/0062-a-subset-of-c.md#findings-and-the-order-the-allocator-grows-in
/// fixed for the allocator, decided in
/// docs/decisions/0108-a-known-zero-high-byte.md.
///
/// Run over the IR of a unit that lowered without errors, before the text is
/// written. It reads no source and produces no finding: a program it cannot
/// narrow is a program it leaves alone.
void narrowRanges( ir::Unit& unit );

} // namespace nga::c
