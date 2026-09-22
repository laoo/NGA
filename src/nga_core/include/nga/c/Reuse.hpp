#pragma once

#include "nga/c/Ir.hpp"

namespace nga::c
{

/// Reads an element once where two blocks in a row would read it twice: a
/// read through an address the block computes, repeated in a block entered
/// from that one alone with nothing written between, is kept in a byte of the
/// Proc's own where it was first read, and the second block reads that byte
/// and computes no address. `if (table[middle] == key) ... if (table[middle] <
/// key)` is the shape. See
/// docs/decisions/0129-an-element-read-twice-on-one-path-is-read-once.md.
void reuseLoads( ir::Unit& unit );

} // namespace nga::c
