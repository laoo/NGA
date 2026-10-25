#pragma once

#include "nga/c/Ir.hpp"
#include "nga/c/Loops.hpp"

namespace nga::c
{

/// Computes once, before a loop, what the loop would compute the same at every
/// turn: an operator whose operands nothing in the loop writes is moved to the
/// block the loop is entered from, its answer kept in a byte of the Proc's
/// own, and every reading of it in the loop reads that byte. `127 - i` in the
/// test of `for (j = 0; j < 127 - i; ...)` is the shape, and `cpx` of one byte
/// is what the test becomes. See
/// docs/decisions/0125-what-a-loop-computes-the-same-every-turn.md.
///
/// A call of the runtime's arithmetic moves as an operator does, and two of
/// the same are kept once; a call of a function of the program writes what
/// `effects` says it does, so that an object it does not write stays
/// invariant across it — see
/// docs/decisions/0304-a-product-a-loop-computes-the-same-is-computed-before-it.md.
void hoistInvariants( ir::Unit& unit, Effects const& effects );

} // namespace nga::c
