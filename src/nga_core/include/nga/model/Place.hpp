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

/// The result of the Place Step: the runtime address of every Section — one
/// address, or for a Movable Section one per Phase it is present in.
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

  /// One address, in every Phase.
  void place( SectionRef where, std::uint32_t address );

  /// This Phase's address, for a Movable Section.
  void place( SectionRef where, PhaseIndex phase, std::uint32_t address );

  [[nodiscard]] bool isPlaced( SectionRef where ) const;

  /// Where the Section stands in one Phase. Ask only of a Phase it is present
  /// in and placed for.
  [[nodiscard]] std::uint32_t addressIn( SectionRef where, PhaseIndex phase ) const;

  /// The one address the Section has across every Phase of `across` it is
  /// placed in — every Phase at all, when `across` is null — or nothing where
  /// those differ. What a Chunk asking from a Module sees, by the rule of
  /// docs/decisions/0030-movable-sections.md.
  [[nodiscard]] std::optional<std::uint32_t> addressAcross( SectionRef where, Residency const* across ) const;

  /// The one address of a Section that has one. A Movable Section standing
  /// at several answers with its lowest Phase's, which no caller may rely on
  /// unless it knows the Section is not one.
  [[nodiscard]] std::uint32_t addressOf( SectionRef where ) const;

private:
  struct Placement
  {
    std::optional<std::uint32_t> everywhere;
    std::vector<std::optional<std::uint32_t>> byPhase;
  };

  [[nodiscard]] Placement const& at( SectionRef where ) const;

  std::vector<std::vector<Placement>> mByModule;
  std::vector<std::optional<std::uint32_t>> mPaneStates;
};

/// The address space a Pane's Sections stand in, once the Layout gave the
/// Pane its state: a number per Bank of a set, whichever Window shows it,
/// and per named state of each Window — where `fixed` and a Window's base
/// state are the one space with no number. Two Sections in different
/// spaces never collide, since a Window shows one state at a time. Nothing
/// for a Pane the Layout could not place.
std::optional<std::uint32_t> addressSpaceOf( Target const& target, Layout const& layout, PaneIndex pane );

/// Sizes and addresses together: what an expression can be evaluated against
/// once both Steps have run.
///
/// An address is asked for from somewhere: a Chunk of a Module sees a Movable
/// Section at the one address it holds across that Module's Residency, and
/// sees nothing where it holds several — which the type check has made
/// impossible for a Chunk, and which an Assertion may still ask.
class Resolved final : public ValueSource
{
public:
  Resolved( Sizes const& sizes, Layout const& layout ) : mSizes( &sizes ), mLayout( &layout ) {}

  /// Whose Residency addresses are asked across from now on.
  void viewFrom( Residency const& residency )
  {
    mViewpoint = &residency;
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
    std::optional<std::uint32_t> const address = mLayout->addressAcross( where, mViewpoint );
    return address.has_value() ? std::optional<std::int64_t>{ *address } : std::nullopt;
  }

  [[nodiscard]] std::optional<std::int64_t> stateOfPane( PaneIndex pane ) override
  {
    std::optional<std::uint32_t> const state = mLayout->stateOfPane( pane );
    return state.has_value() ? std::optional<std::int64_t>{ *state } : std::nullopt;
  }

private:
  Sizes const* mSizes;
  Layout const* mLayout;
  Residency const* mViewpoint = nullptr;
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
/// A Movable Section is a Claim per Phase, held to one address wherever the
/// Freezes say a Reference sees it, and the solver moves it between Phases
/// only where that lets a layout exist — every move is bytes copied at run
/// time, and the fewest of those is what it minimises. See
/// docs/decisions/0030-movable-sections.md.
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

} // namespace nga::model
