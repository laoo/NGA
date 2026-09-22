#pragma once

#include "nga/diag/DiagnosticSink.hpp"
#include "nga/model/Build.hpp"
#include "nga/model/Module.hpp"

#include <span>
#include <vector>

namespace nga::model
{

/// The result of the Prune Step: every Section reached from a Root, and so
/// kept. A Section not here is dropped — placed by nothing, written by
/// nothing, loaded by nothing — and its Chunks are still typed and sized,
/// because an error in code nothing reaches is still an error in the code as
/// written. Keyed like every Step result.
class Reachable
{
public:
  /// Nothing reached yet.
  explicit Reachable( std::span<Module const> modules );

  /// Every Section: what a program with no Root keeps, since reachability
  /// needs somewhere to start from.
  static Reachable all( std::span<Module const> modules );

  /// True when the Section was not reached before.
  bool keep( SectionRef where );

  [[nodiscard]] bool includes( SectionRef where ) const;

private:
  std::vector<std::vector<bool>> mByModule;
};

/// The Prune Step: reachability over References from the Roots.
///
/// The Roots are the entry Phase's entry Label and every Section declared
/// `root`. From there a Section is reached by a Chunk that encodes its
/// address — the walk of `References.hpp`, which is also the type check's — and
/// the Chunks of the Target's own kinds reach what their bytes will name: a
/// `.transition` the routine and the entry and table of the Phase it enters,
/// the Cell the entry Phase's table, a Slot's Cell every Implementation the
/// edges may write into it. A table reaches nothing — not the Phases its
/// edges enter, since an edge no `.transition` names is never selected, and
/// not what it loads, since the load set is what reachability filters, or
/// nothing in a Module an edge loads could ever be dropped.
///
/// A program that declares no Root keeps everything. A pinned Section that
/// nothing reaches is dropped and warned about, because a pin is a claim that
/// something reaches that address, and the something may be the hardware.
/// See docs/decisions/0033-prune.md.
Reachable prune( Merged const& build, diag::DiagnosticSink& sink );

/// What `.root` takings declare, resolved once every name is: the Section
/// an address is handed to the hardware from becomes a Root, kept by Prune,
/// active in every state for Trace, and held to docs/decisions/0040-root-evicted.md.
/// After Expand, since a taking inside a macro is its expansion's, and before
/// Prune, whose Roots these are — see docs/decisions/0061-root-at-the-taking.md.
/// A `.root` that names no Section marks nothing, and is warned about.
void markRoots( diag::SourceManager const& sources,
                GlobalSymbols const& symbols,
                std::span<Module> modules,
                diag::DiagnosticSink& sink );

} // namespace nga::model
