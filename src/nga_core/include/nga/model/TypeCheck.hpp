#pragma once

#include "nga/diag/DiagnosticSink.hpp"
#include "nga/model/Build.hpp"
#include "nga/model/Merge.hpp"
#include "nga/model/Types.hpp"
#include "nga/syntax/Expression.hpp"

#include <span>
#include <vector>

namespace nga::model
{

/// The type of one expression, as written in one Module.
///
/// Every rule of docs/decisions/0010-expression-types.md is a rule about types
/// and none of them needs a value: `label - label` is an Integer, `label + 1`
/// is an Address and `label + label` is an error, all decided while no address
/// exists anywhere in the program.
///
/// Runs after Merge, because a name may come from a Module this one never saw,
/// and a Section identity or a PlacementClass with it. Returns UNKNOWN once
/// something has been reported, so one bad operand costs one finding.
Type typeOf( Merged const& build, ModuleIndex home, syntax::Expression const& node, diag::DiagnosticSink& sink );

/// For every Movable Section, the Residencies across which a direct Reference
/// holds it to one address: a Chunk encodes the address it sees, and it sees
/// one in every Phase it is present in, so the Section may not move between
/// those Phases. A Reference from the Section's own Module freezes it across
/// its whole Residency, which is why a Section that names itself does not
/// move at all. See docs/decisions/0030-movable-sections.md.
///
/// Produced by the type check, which is where References are resolved, and
/// read by Place and by the layout verifier. Keyed like every Step result.
class Freezes
{
public:
  explicit Freezes( std::span<Module const> modules );

  void freeze( SectionRef where, Residency const& across );

  [[nodiscard]] std::span<Residency const> of( SectionRef where ) const;

  /// The second thing the type check learns for Place: a Section that runs,
  /// or is named, while a `.with` shows some state of a Window is not seen
  /// there if it stands in that Window's base state, so Place keeps it out
  /// of the Window's ranges — see docs/decisions/0055-with.md.
  void keepOut( SectionRef where, WindowIndex window );

  [[nodiscard]] std::span<WindowIndex const> keptOutOf( SectionRef where ) const;

private:
  std::vector<std::vector<std::vector<Residency>>> mByModule;
  std::vector<std::vector<std::vector<WindowIndex>>> mKeepOut;
};

/// Types every expression of the program, and applies the rules that belong to
/// the statement holding one rather than to the expression itself — which item
/// types a data directive takes, and that a reservation counts something.
///
/// Also the one rule about where a Reference may point: from a Section whose
/// Residency is R to memory whose Residency is S only where R ⊆ S, so that the
/// target is there whenever the referrer is. The Phases are what names the one
/// where it is not. See docs/decisions/0028-references-across-residency.md.
///
/// The same walk over References is what finds where a Movable Section is
/// held still, which is the result returned — and, with the `.with` rules of
/// docs/decisions/0055-with.md, which Sections Place keeps out of a Window.
Freezes checkTypes( Pruned const& build, diag::DiagnosticSink& sink );

} // namespace nga::model
