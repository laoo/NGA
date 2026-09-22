#pragma once

#include "nga/c/Ir.hpp"

namespace nga::c
{

/// Reads a byte a constant was just stored into as that constant, and makes a
/// constant of an operator whose operands are all constants — the two
/// together and to a fixed point, since each makes the other's next site. A
/// body spliced where its call stood reads its arguments from bytes, and this
/// is what takes the constant back out of one; the store that loses its last
/// reader is `dropDeadStores`'s to remove. See
/// docs/decisions/0175-a-constant-reaches-its-reader.md, whose table is what
/// a constant may not be carried across.
void foldConstants( ir::Unit& unit );

} // namespace nga::c
