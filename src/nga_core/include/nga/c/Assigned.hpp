#pragma once

#include "nga/c/Ir.hpp"
#include "nga/diag/DiagnosticSink.hpp"

namespace nga::c
{

/// Reports a `static` local of a function read where some path from the
/// function's entry reaches the read without writing it.
///
/// A `static` local holds what was there — see
/// docs/decisions/0197-a-global-nobody-assigned-is-not-initialised.md — so the
/// first call reads a byte nobody wrote, and the program's own `firstTime` and
/// `count = count + 1` are the two shapes of that mistake. The analysis is
/// sound here and is not for a global, because **nothing else can write the
/// byte**: the name is the compiler's, under the function's, exported by
/// nothing, and a program may not spell one beginning with `__`. So "nobody has
/// written it yet" is a fact about one function's control flow rather than
/// about the whole program, which is what 0197 could not say of a global and
/// why it left a global to the machine's poison instead.
///
/// The rule costs a program one initialiser where the read is unreachable on
/// the first call for a reason the flow does not carry — a second `static`
/// standing as a flag. That case is a byte of the image and no cycle, and the
/// reasoning is
/// docs/decisions/0212-a-static-local-read-before-it-is-written.md.
///
/// Over the freshly lowered IR, before any pass moves or drops a store.
void checkStaticsAreAssigned( ir::Unit const& unit, diag::DiagnosticSink& sink );

} // namespace nga::c
