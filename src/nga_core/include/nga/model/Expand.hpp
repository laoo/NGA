#pragma once

#include "nga/diag/DiagnosticSink.hpp"
#include "nga/diag/SourceManager.hpp"
#include "nga/model/Merge.hpp"
#include "nga/model/Module.hpp"
#include "nga/model/Project.hpp"

#include <span>

namespace nga::model
{

/// The Step that decides every `.if` and gives every macro use its inside.
///
/// One rule, applied at every level: a conditional is decided **before** what
/// it holds is instantiated. A Section's conditionals are answered first and
/// the Chunks of every branch not taken are left occupying nothing; a use
/// that survives is then expanded, and inside it the body's conditionals are
/// answered — with this use's arguments in them — before any Chunk of the
/// body is cloned, so a nested use in a branch not taken is never resolved
/// and never expanded. That is what lets a macro expand itself: the branch
/// holding the recursive use dies when the argument says so. A use standing
/// in a branch the build excluded is not looked at, and no finding is raised
/// about it: a finding about code that is not there is a finding about
/// nothing.
///
/// Its position is fixed at both ends. After Merge and Charsets, because a
/// condition is a **declared value** and a use is looked up as any Symbol
/// is, neither of which can be settled until every Module's Symbols are
/// known. Before Prune and the type check, because a Reference in a branch
/// that is not there would otherwise keep its Section from being dropped,
/// and because both read the inside of a use where they read a Chunk.
///
/// It changes Chunks alone. A use is one Chunk whose inside is kept beside
/// the Section's arena, a body's labels are not Symbols, and no definition
/// may stand in a branch, so the table Merge produced stays exactly true —
/// a Step between Merge and Prune may finish Chunks, never Symbols.
///
/// An argument that has a declared value is substituted as that value, held
/// on a VALUE node with the argument's span, so that a recursion's argument
/// stays one node deep however far it goes; one written `U + c`, `c + U` or
/// `U - c` with `c` declared is carried as what U stands for and the sum, so
/// that one reaching a position stays two. Every tree it builds is held to
/// syntax::MAX_EXPRESSION_DEPTH — see docs/decisions/0070-expression-depth.md.
/// An instantiated node names what
/// the macro's Module sees, and one cloned from an argument what the use's
/// Module sees; the use's Module records which is which — see
/// Module::homeOf. A local label of the body becomes a position inside the
/// use's Chunk. A qualified use, `nga.read`, is a role of the driver the
/// Project resolved, reached from the declaration and not by name.
///
/// See docs/decisions/0049-recursive-macros.md, which supersedes the place
/// 0043 gave expansion and the two Steps 0048 decided `.if` in.
void expand( diag::SourceManager const& sources,
             GlobalSymbols const& symbols,
             Charsets const& charsets,
             Project const& project,
             std::span<Module> modules,
             diag::DiagnosticSink& sink );

} // namespace nga::model
