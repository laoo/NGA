#pragma once

#include "nga/diag/DiagnosticSink.hpp"
#include "nga/model/Build.hpp"
#include "nga/model/Module.hpp"

#include <span>
#include <vector>

namespace nga::model
{

/// The result of the ReadOnly Step: every Section nothing writes, which is
/// what may stand in a `rom` Region. Keyed like every Step result.
class ReadOnly
{
public:
  explicit ReadOnly( std::span<Module const> modules );

  void mark( SectionRef where, bool inRom );

  /// Whether nothing writes the Section.
  [[nodiscard]] bool includes( SectionRef where ) const;

  /// Whether it therefore stands in ROM: the Target has a `rom` Region for it,
  /// or a pin puts it inside one.
  [[nodiscard]] bool standsInRom( SectionRef where ) const;

private:
  std::vector<std::vector<bool>> mByModule;
  std::vector<std::vector<bool>> mInRom;
};

/// The ReadOnly Step: which Sections may stand in memory no code writes.
///
/// A Section is read-only when **nothing writes it**: no Reference of kind
/// `Write` or `ReadWrite` from a Section Prune kept names any of its Labels,
/// and no address of it is taken. A `.proc` is excepted from the second half
/// and not the first, because an address of code taken is a jump and never a
/// write — `.own` says so — while a store into a Proc is self-modifying code
/// and is seen like any other store.
///
/// Where an address **is** taken, the Section says `readonly`, which declares
/// that the pointer is read through: `.own` and `.root` say who follows an
/// address and never whether they write through it, so a display list handed
/// to `SDLSTL` and a screen buffer handed to a copy loop are one statement to
/// the tool. The word is declared and not checked, as `temporary` is, but it
/// cannot lie about what the tool *can* see: a `Write` Reference to a Section
/// that says it is refused.
///
/// A Section of reservations alone is never read-only — it has no bytes for
/// ROM to hold — nor is a `zeropage` one, a Movable one or a Temporary, each
/// refused with the word where the word was written.
///
/// **Standing in ROM** is the answer that follows: a read-only Section whose
/// Target has a `rom` Region for it, or one pinned inside one. It is asked
/// here and nowhere else, because three Steps read it and they must not
/// disagree — PlaceStorage, so that no Transition tries to copy into ROM;
/// Place, for the pool; and the layout verifier, for the pool it holds a
/// Section to.
///
/// Runs after Trace, which is the last Step that reads what a Chunk does to
/// memory, and before Size: what it needs of a Section is whether any Chunk of
/// it emits, which is the Section's own answer. See
/// docs/decisions/0215-a-cartridge-is-rom-and-a-section-stands-in-it-when-nothing-writes-it.md.
ReadOnly readOnlySections( Pruned const& build, diag::DiagnosticSink& sink );

} // namespace nga::model
