#pragma once

#include "nga/diag/DiagnosticSink.hpp"
#include "nga/model/Build.hpp"
#include "nga/model/TypeCheck.hpp"

namespace nga::model
{

/// The rules of `.with` and of a Pane's visibility, over the References the
/// type check walks — see docs/decisions/0055-with.md and 0054.
///
/// A Section in a Pane is seen only while its Pane is shown. So a Reference
/// into a Pane from a statement under no `.with` is sound only from the
/// Pane's own code (`NGA2412`); and a `.with` is sound iff everything its
/// statement reaches — the statement's own References, and through every
/// Call and Jump the Sections they reach, with the walk Prune makes — names
/// no Pane but the ones shown and stands in none but those (`NGA3008`). A
/// callee inherits nothing: what it needs is read off its own References.
///
/// The second thing learned is for Place: every Section a `.with` on Window
/// W runs or names, and that is in no Pane of W, would vanish if it stood in
/// W's base state, so it is kept out of W's ranges — recorded in the Freezes
/// and read by Place and the layout verifier.
void checkWiths( Pruned const& build, Freezes& freezes, diag::DiagnosticSink& sink );

} // namespace nga::model
