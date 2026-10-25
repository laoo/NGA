#pragma once

#include "nga/diag/DiagnosticSink.hpp"
#include "nga/model/Build.hpp"
#include "nga/model/Evaluate.hpp"
#include "nga/model/Merge.hpp"
#include "nga/model/ReadOnly.hpp"
#include "nga/model/Size.hpp"
#include "nga/model/Storage.hpp"
#include "nga/model/TypeCheck.hpp"

#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace nga::model
{

/// The result of the Place Step: the runtime address of every Section, one
/// in every Phase it is present in.
///
/// Held apart from the Sections, which is what makes the loop from address to
/// size to PlacementClass unwritable rather than merely forbidden — while Size
/// runs, this does not exist. See
/// docs/decisions/0012-step-results-are-separate-objects.md.
class Layout
{
public:
  explicit Layout( std::span<Module const> modules, std::size_t paneCount = 0 );

  /// The state of its Window the solver gave a Pane — the first of a
  /// family's. See docs/decisions/0054-panes.md.
  void placePane( PaneIndex pane, std::uint32_t state );
  [[nodiscard]] std::optional<std::uint32_t> stateOfPane( PaneIndex pane ) const;

  void place( SectionRef where, std::uint32_t address );

  [[nodiscard]] bool isPlaced( SectionRef where ) const;

  /// Where the Section stands, in every Phase it is present in. Ask only of
  /// one that is placed.
  [[nodiscard]] std::uint32_t addressOf( SectionRef where ) const;

private:
  std::vector<std::vector<std::optional<std::uint32_t>>> mByModule;
  std::vector<std::optional<std::uint32_t>> mPaneStates;
};

/// The address space a Pane's Sections stand in, once the Layout gave the
/// Pane its state: a number per Bank of a set, whichever Window shows it,
/// and per named state of each Window — where `fixed` is the one space
/// with no number. Two Sections in different
/// spaces never collide, since a Window shows one state at a time. Nothing
/// for a Pane the Layout could not place.
std::optional<std::uint32_t> addressSpaceOf( Target const& target, Layout const& layout, PaneIndex pane );

/// Sizes and addresses together: what an expression can be evaluated against
/// once both Steps have run.
class Resolved final : public ValueSource
{
public:
  Resolved( Sizes const& sizes, Layout const& layout ) : mSizes( &sizes ), mLayout( &layout ) {}

  /// Where the Target and the Modules are known as well, a Pane's Section
  /// has a linear address too.
  Resolved( Sizes const& sizes, Layout const& layout, Target const& target, std::span<Module const> modules )
      : mSizes( &sizes ), mLayout( &layout ), mTarget( &target ), mModules( modules )
  {
  }

  [[nodiscard]] std::optional<std::int64_t> sizeOfSection( SectionRef where ) override
  {
    return mSizes->isKnown( where ) ? std::optional<std::int64_t>{ mSizes->sizeOfSection( where ) } : std::nullopt;
  }

  [[nodiscard]] std::optional<std::int64_t>
  offsetOf( SectionRef where, ChunkIndex chunk, std::optional<std::uint32_t> inner ) override
  {
    if ( !mSizes->isKnown( where ) )
    {
      return std::nullopt;
    }
    return inner.has_value() ? mSizes->innerOffsetOf( where, chunk, *inner ) : mSizes->offsetOf( where, chunk );
  }

  [[nodiscard]] std::optional<std::int64_t> addressOf( SectionRef where ) override
  {
    if ( !mLayout->isPlaced( where ) )
    {
      return std::nullopt;
    }
    return mLayout->addressOf( where );
  }

  [[nodiscard]] std::optional<std::int64_t> stateOfPane( PaneIndex pane ) override
  {
    std::optional<std::uint32_t> const state = mLayout->stateOfPane( pane );
    return state.has_value() ? std::optional<std::int64_t>{ *state } : std::nullopt;
  }

  [[nodiscard]] std::optional<std::int64_t> linearAddressOf( SectionRef where ) override;

private:
  Sizes const* mSizes;
  Layout const* mLayout;
  Target const* mTarget = nullptr;
  std::span<Module const> mModules{};
};

/// The Place Step. Every Section becomes a Claim and the solver is asked about
/// all of them at once, searching in Project order from the lowest address —
/// pins first — so that a layout that exists is found, and one run's layout is
/// the next one's. See docs/decisions/0029-cp-sat-and-or-tools.md.
///
/// A pin is a Constraint the solver is obliged to satisfy, so landing in memory
/// it would otherwise have allocated from is not an error — it sees the pin.
///
/// A Section with a Payload is fitted outside the Target's Window, and a pin
/// that puts one inside it is an error: while a Bank is switched in for the
/// copy, an address in the Window is the Bank. See
/// docs/decisions/0017-payloads-and-banks.md.
///
/// Before the solver is asked, what every Phase's Sections take of each pool
/// is summed against what the pool holds, and a Phase that outweighs one is
/// reported with the Sections that weigh most: the first layer of
/// docs/spec/diagnostics.md, arithmetic over the model that names the cause
/// where the solver could only name whichever Section came last. The solver
/// is asked all the same, so that a Section nothing could hold on its own is
/// reported where it stands.
///
/// When no layout exists and `explain` is set, the solver is asked once more
/// with the no-overlap made soft, and answers with the fewest pairs of
/// Sections some layout has to let collide — unless the sums already refused
/// a Phase, in which case a pair explains nothing the sum did not.
Layout placeSections(
    Sized const& build, Storage const& storage, ReadOnly const& readOnly, bool explain, diag::DiagnosticSink& sink );

/// The Assertions of every Module, which are checked **after** Place because
/// that is the whole point of them: they guard assumptions the solver knows
/// nothing about.
void checkAssertions( Placed const& build, diag::DiagnosticSink& sink );

/// Whether the program can keep bytes hidden under a ROM at all: a `.xex`
/// without storage, whose driver switches its ROMs and masks the machine's
/// interrupts while the load has them out — see
/// docs/decisions/0241-bytes-kept-for-a-later-phase-may-lie-hidden-under-a-rom.md.
[[nodiscard]] bool canHide( Merged const& build );

/// Whether a Module is a ROM the driver switches out.
[[nodiscard]] bool isSwitchedRom( Merged const& build, ModuleIndex module );

/// Whether `section` may stand under `rom`, a Section of a ROM the driver
/// switches out: in every Phase both are present in, `section` is held for a
/// later Phase, its Module is not needed and nothing there names it. Its bytes
/// are hidden there.
[[nodiscard]] bool hidesUnder( Sized const& build, SectionRef section, SectionRef rom );

/// The Phases a Section's bytes are held apart in: what `heldAcross` says of
/// its Residency, with every Phase the hardware may hold it in counted as
/// one that needs it — see
/// docs/decisions/0247-the-hardware-holds-a-section-for-as-long-as-it-is-given-it.md.
[[nodiscard]] Residency heldAcrossOf( Traced const& build, SectionRef where );

/// Whether a Section is the machine's memory while a `.xex` loads: pinned,
/// with no bytes, in a Module the `load` block needs.
[[nodiscard]] bool isMachineAtLoad( Merged const& build, SectionRef section );

/// Whether the load Step places a Section rather than Place: `nga.load`'s, and
/// what else a Module the `load` block needs holds — the program's own code
/// for the load, which runs before the start is written and is overwritten by
/// it — see
/// docs/decisions/0246-the-load-of-a-xex-needs-what-the-machine-holds-then.md.
///
/// Code of the load's that a Holder holds into a Phase is that Phase's, and
/// Place places it — see
/// docs/decisions/0247-the-hardware-holds-a-section-for-as-long-as-it-is-given-it.md.
/// So is a Section in a Pane or a unit set, which the units write — see
/// docs/decisions/0261-a-section-of-the-load-in-a-medium-is-written-with-the-units.md.
[[nodiscard]] bool placedByTheLoad( Traced const& build, SectionRef section );

/// Whether a Section is the program's own code for the load: placed by the
/// load Step and none of the tool's — see
/// docs/decisions/0246-the-load-of-a-xex-needs-what-the-machine-holds-then.md.
[[nodiscard]] bool isLoadCode( Traced const& build, SectionRef section );

/// A range of memory the machine holds while a `.xex` loads, and the Section
/// of the Module the load needs that says so.
struct HeldWhileLoading
{
  AddressRange range;
  SectionRef holder;
};

/// What the machine holds while a `.xex` loads: every pinned Section with no
/// bytes of the Modules the `load` block names. Nothing for another
/// Container — see
/// docs/decisions/0246-the-load-of-a-xex-needs-what-the-machine-holds-then.md.
[[nodiscard]] std::vector<HeldWhileLoading> heldWhileLoading( Sized const& build );

/// Whether the `.xex` writes this Section's bytes while it loads, so that
/// nothing the machine holds then may stand under it: a Section with bytes
/// present in the entry Phase.
[[nodiscard]] bool writtenWhileLoading( Sized const& build, SectionRef section );

} // namespace nga::model
