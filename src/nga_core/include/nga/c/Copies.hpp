#pragma once

#include "nga/c/Ir.hpp"

namespace nga::c
{

/// Reads a byte of the Proc's own that a copy of an object filled as that
/// object, where the copy then has no reader left: `x = place0[0]` read three
/// times as `x` is read three times as `place0`, and the copy goes. Only where
/// every reader the copy reaches takes it, along the straight line a constant
/// is carried on, with nothing between that may write the object; and only
/// where that costs less than the copy did. See
/// docs/decisions/0267-a-copy-read-few-times-is-read-from-where-it-came.md.
void propagateCopies( ir::Unit& unit );

} // namespace nga::c
