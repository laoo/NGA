#pragma once

#include "nga/c/Ir.hpp"

#include <optional>
#include <span>

namespace nga::c
{

/// Gives the bytes of a function's result to a local the function returns, so
/// that the copy standing at the end of it is not written: in
/// `u16 t; t = x + y; return t;` the sum is computed into `__ret` and the
/// local is gone, and a `struct` built in a local is built in the result.
///
/// Only a local — never a parameter, whose name the caller writes and which
/// would need the result and an argument to be one Temporary — and only where
/// every write of the result is the last thing the function does, which is
/// what makes the local's bytes free to be the result's from the first write
/// on. See docs/decisions/0117-a-returned-local-is-the-result.md.
void mergeReturnedLocals( ir::Unit& unit );

/// Gives the bytes of a function's result to a parameter the function
/// returns, where no local took them first: `u16 f(u16 x) { x = x + 1;
/// return x; }` leaves its answer in `x` and writes no copy, and the Proc
/// declares that one reservation both an argument and the result — see
/// docs/decisions/0119-one-temporary-carries-two-roles.md.
///
/// Over the whole program and after every body is lowered, because the byte
/// the result lies in is part of the Signature every caller reads: the calls
/// of every unit are told where to read it. A function whose result the
/// caller cannot know for certain is left alone — a member of a function
/// type, one that implements a Slot, one a `switch` reaches. See
/// docs/decisions/0120-a-function-returns-through-a-parameter.md.
void returnThroughParameters( std::span<std::optional<ir::Unit>> units );

} // namespace nga::c
