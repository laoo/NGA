#pragma once

#include "nga/diag/DiagnosticSink.hpp"
#include "nga/model/Build.hpp"
#include "nga/model/Merge.hpp"
#include "nga/model/Types.hpp"
#include "nga/syntax/Expression.hpp"
#include "nga/syntax/Operand.hpp"

#include <cstdint>
#include <optional>
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

/// What an instruction's operand, written in Module `home` in `shape`, makes
/// of the instruction's width: its type and, for an Integer, the value the
/// author declared, asked quietly, and the PlacementClass these give —
/// nothing where they give none, which Size is the one to report. Only a
/// direct shape has two widths to choose between, so of any other shape
/// nothing is asked. Size sizes an instruction by it, and the type check
/// knows by it how many bytes an operand the program writes in place has.
struct OperandPlacement
{
  Type type{};
  std::optional<std::int64_t> value{};
  std::optional<syntax::PlacementClass> placement{};
};

[[nodiscard]] OperandPlacement operandPlacement( Merged const& build,
                                                 ModuleIndex home,
                                                 syntax::Expression const& operand,
                                                 syntax::OperandShape shape );

/// Whether `site`, written in Module `home`, names a byte of code the program
/// may write in place: within the operand of the instruction at the Label
/// `target`, where that instruction does not jump and its operand names no
/// Temporary directly, or its opcode where `.alt` declares what else it may
/// be — but not the start of a Proc, whose address is a way in. Taken without
/// `.own` or `.root`, such an address is data, which nobody follows — see
/// docs/decisions/0238-the-address-of-a-byte-the-program-may-write-is-data.md.
[[nodiscard]] bool
namesSettableByte( Merged const& build, ModuleIndex home, SymbolRef target, syntax::Expression const& site );

/// Whether an address `site` takes through the Label `target`, with nothing
/// said, is data rather than a way in: a byte the program may write, as
/// `namesSettableByte` says, or a position reached through a Label that
/// stands at data or a reservation in a `.section`, though the Section holds
/// code too. Inside a Proc only the first — see
/// docs/decisions/0240-an-address-of-data-in-a-section-of-code-is-data.md.
[[nodiscard]] bool
takenAsData( Merged const& build, ModuleIndex home, SymbolRef target, syntax::Expression const& site );

/// For every Section, the Phases a Reference to it is made from, of any kind:
/// the Residency of every Module whose Section names it. Where a Section is
/// named its bytes must be visible, so it is hidden under a ROM nowhere in
/// these — see docs/decisions/0241-bytes-kept-for-a-later-phase-may-lie-hidden-under-a-rom.md.
///
/// Produced by the type check, which is where References are resolved, and
/// read by Place. Keyed like every Step result.
class Referrers
{
public:
  explicit Referrers( std::span<Module const> modules );

  void add( SectionRef where, Residency const& from );
  [[nodiscard]] Residency const& of( SectionRef where ) const;

private:
  std::vector<std::vector<Residency>> mByModule;
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
/// The same walk over References is what finds the Phases each Section is
/// named from, which is the result returned; the `.with` rules of
/// docs/decisions/0055-with.md are checked over the same walk.
Referrers checkTypes( Pruned const& build, diag::DiagnosticSink& sink );

} // namespace nga::model
