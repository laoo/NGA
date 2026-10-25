#pragma once

#include "nga/model/Project.hpp"

#include <cstdint>
#include <optional>
#include <span>
#include <utility>
#include <vector>

namespace nga::model
{

/// One Section's demand on address space, as the solver sees it: a size, the
/// starts its Constraints allow, and a Residency. See the glossary.
///
/// Every Constraint reaches the solver as a restriction on where the Claim
/// may start — a pin is one range of one start, zero page is what ends below
/// $100, a Section with a Payload has the Window cut out of its pool, an
/// alignment is a stride, a boundary is a bound on the start within its
/// block — so the solver knows no Constraint by name. A Constraint that is
/// impossible on its own, a pin past the end of memory or two pins at one
/// address in one Phase, is reported above the solver, by the Step that still
/// knows what the Constraint was called.
struct Claim
{
  std::uint32_t size = 0;

  /// Half-open ranges of **start** addresses, sorted and disjoint: the
  /// Section's bytes lie within its pool wherever it starts in one of them.
  /// Empty when nothing the Constraints allow is large enough, which is
  /// answered as unplaced without asking the solver.
  std::vector<AddressRange> allowedStarts;

  /// The start is a multiple of this. One means anywhere.
  std::uint32_t alignment = 1;

  /// The bytes do not cross a multiple of this. Zero means no such bound;
  /// otherwise it is at least the size, which the caller has made sure of.
  std::uint32_t boundary = 0;

  Residency residency;

  /// The Claims, by position in the list, this one may overlap in any Phase
  /// both are present in: two Temporaries never live at once — see
  /// docs/decisions/0034-trace.md — and each naming the other; or a ROM's
  /// Claim and what hides under it, named on the ROM's side alone, since a
  /// Claim naming any partner is held apart pair by pair rather than in the
  /// no-overlap — see
  /// docs/decisions/0241-bytes-kept-for-a-later-phase-may-lie-hidden-under-a-rom.md.
  std::vector<std::size_t> sharesWith;

  /// The Claim, by position, this one stands immediately after: its start
  /// is that Claim's start plus size. A Proc
  /// that another falls through into.
  std::optional<std::size_t> follows;

  /// Which state of which Window the Claim stands in: nothing for `fixed`,
  /// which is one address space, and a number
  /// per named state a Pane was pinned to. Two Claims in different groups
  /// never collide, since a Window shows one state at a time — the
  /// co-visibility rule of docs/decisions/0052-a-view-is-a-state-of-a-window.md.
  /// Nothing for a Claim in a Pane the solver gives a Bank, whose space is
  /// the Bank it decides — see `pane`.
  std::optional<std::uint32_t> group{};

  /// The Pane the Claim stands in, by position in the Problem's list, for
  /// one the solver gives a Bank: the Claim collides with what shares that
  /// Bank at the same offset within the Window, and with nothing else.
  std::optional<std::size_t> pane{};

  /// The unit set, by the Target's index, a Claim stands in as the medium
  /// sees it: its start is a linear address, its bytes may run across the
  /// set's Banks, and it collides with what else stands on the set's axis —
  /// see docs/decisions/0254-a-section-may-stand-in-a-unit-set.md.
  std::optional<std::uint32_t> linear{};
};

/// One unit set as one axis: where its first Bank starts and how much each
/// holds, so that a Pane's Section at an offset in Bank `b` stands at
/// `begin + b * bankSize + offset`, the coordinate a Section of the set itself
/// has. A set that declares no `linear` starts at zero, which no Claim of
/// another kind ever meets.
struct Axis
{
  std::uint32_t begin = 0;
  std::uint32_t bankSize = 0;
};

/// A Pane as the solver sees it: which set of Banks it may have, how many of
/// them it takes at once, and the Window whose offset its Sections are
/// measured in — so that two Windows over one set are one coordinate
/// system. See docs/decisions/0054-panes.md.
struct PaneClaim
{
  /// The unit set, by the Target's index; Panes of different sets never
  /// collide.
  std::uint32_t set = 0;

  /// How many Banks the set has: the Pane's Bank is one of `0 .. banks - 1`,
  /// and a family of `count` takes `count` consecutive ones, its Sections
  /// standing in each at one offset.
  std::uint32_t banks = 0;
  std::uint32_t count = 1;

  /// The Window's ranges, sorted: a Section's offset is its distance into
  /// them laid end to end, which is what an address in a Bank means
  /// whichever Window shows it.
  std::vector<AddressRange> window{};
};

/// Bytes of a Bank that are taken before the solver is asked — a Frame,
/// which waits in storage from the start of it — and that a Pane's Section
/// may not stand on in a Phase they are live in.
struct Occupied
{
  std::uint32_t set = 0;
  std::uint32_t bank = 0;
  std::uint32_t offset = 0;
  std::uint32_t size = 0;

  /// The Phases the bytes are taken in; one over no Phases at all means
  /// every Phase.
  Residency live{};
};

/// Everything one question to the solver holds.
struct Problem
{
  std::vector<Claim> claims;
  std::vector<PaneClaim> panes;
  std::vector<Occupied> occupied;

  /// Per unit set, by the Target's index: its axis. Read for every set a
  /// PaneClaim, a Claim's `linear` or an Occupied names.
  std::vector<Axis> axes{};

  /// True when the sums over the model have already refused some Phase or
  /// Pane. Then no layout exists, and the solver is not asked to prove it:
  /// a sum it does not see can take CP-SAT longer than any build waits.
  bool refused = false;
};

/// The solver's answer, in the order the Claims were given.
struct Solution
{
  /// False when no layout satisfies every Claim at once. Then no Claim the
  /// solver was asked about has an address, and which of them collide is a
  /// question the solver cannot answer in this form — see
  /// docs/spec/diagnostics.md on explaining an infeasible layout.
  bool feasible = true;

  /// False when the search ran out of its budget before it found a layout or
  /// showed that none exists; then `feasible` is false too, and nothing says
  /// that no layout exists — see
  /// docs/decisions/0255-the-portfolio-has-a-budget-too.md.
  bool decided = true;

  /// The one address of each Claim: nothing for a Claim with no allowed
  /// start, and for every Claim when the whole is infeasible.
  std::vector<std::optional<std::uint32_t>> addresses;

  /// The Bank of each Pane, in the order the Panes were given — the first
  /// of a family's — and nothing when the whole is infeasible.
  std::vector<std::optional<std::uint32_t>> paneBanks;
};

/// How much deterministic time raising the Panes' Banks may take, once a
/// layout is known to exist. Deterministic time counts the solver's work and
/// not the clock, which is why a budget on it gives the same answer on every
/// machine — the one kind of limit that ever belongs here. The fixed search
/// tries the highest Bank first, so the layout found is mostly as high as
/// any; what a larger budget buys is the proof of it, which where Panes cannot
/// share a Bank is a pigeonhole CP-SAT does not close. Running out leaves the
/// layout found, which is not a finding. The fixed search raises them
/// whichever search found the layout. See
/// docs/decisions/0249-raising-the-panes-has-a-budget-of-its-own.md and
/// docs/decisions/0257-the-portfolio-ends-with-its-batch-and-the-raise-is-the-fixed-search.md.
constexpr double RAISE_BUDGET = 0.5;

/// How much deterministic time the fixed search may take to find a layout,
/// before CP-SAT's portfolio is asked instead. The slowest program of the
/// suite takes about a hundredth of it, so running out means the fixed search
/// is lost rather than slow.
constexpr double FIND_BUDGET = 0.5;

/// How much deterministic time the portfolio may take after it. No program
/// known has needed more than a few thousandths, and one that ran on for
/// minutes had no layout to find: CP-SAT proves a packing infeasible no
/// faster than it searches it. Running out is a finding of its own, which
/// says that no layout was found and not that none exists. The limit is
/// overrun by the portfolio's batches, and it comes to about two seconds on
/// a model of seven hundred Claims. See
/// docs/decisions/0255-the-portfolio-has-a-budget-too.md.
constexpr double PORTFOLIO_BUDGET = 0.25;

/// How many subsolvers the portfolio interleaves, each on a thread, in
/// batches that wait for one another. A constant, and not the machine's
/// cores, so that the layout is the same on every machine; and no batch is
/// cut short by the first task to find a layout, so that it is the same on
/// every run — see
/// docs/decisions/0257-the-portfolio-ends-with-its-batch-and-the-raise-is-the-fixed-search.md.
constexpr int PORTFOLIO_WORKERS = 8;

/// Every Claim at once, as a constraint model: a start variable per Claim
/// over its allowed starts, and one no-overlap
/// per Phase over the Claims present in it: the co-visibility rule of
/// docs/decisions/0002-memory-model.md stated once per Phase rather than once
/// per pair. A Claim that may share with another cannot be in a no-overlap,
/// which excepts no pair, so it is held apart from every other Claim of the
/// Phase pair by pair, except the ones it names. A Pane has a Bank variable,
/// and its Claims are rectangles of offset by Bank — a family's as tall as
/// it is wide — in one two-dimensional no-overlap per Phase and set, with
/// the bytes already occupied there. A set some Claim stands in itself is
/// also one axis, every Claim of it one interval there — a Pane's at its
/// Bank's start plus its offset, once per member — since that Claim may run
/// across Banks. A layout that exists is found; where
/// there is a choice, the one whose Panes have the highest Banks, so that
/// storage, packed from the bottom, meets them late.
///
/// The search is fixed: highest Bank first, Claims in
/// the order given, lowest start first, on one worker and with no limit on
/// the clock. That is what makes one run's answer the next one's, and it is
/// why the order is the caller's to choose. See
/// docs/decisions/0029-cp-sat-and-or-tools.md. Where it runs out of
/// FIND_BUDGET, CP-SAT's portfolio searches instead, interleaved in fixed
/// batches each run to its end, which is as deterministic and keeps no order —
/// see
/// docs/decisions/0244-the-fixed-search-has-a-budget-and-a-portfolio-after-it.md
/// — for at most PORTFOLIO_BUDGET.
Solution solve( Problem const& problem );

/// The Claims alone, with no Pane among them.
Solution solve( std::span<Claim const> claims );

/// Which Claims cannot be kept apart: the pairs a layout has to overlap when
/// it may overlap as few as possible, given by their positions in the list.
struct Explanation
{
  /// False when the search ran out of its budget before showing that fewer
  /// pairs would do: the pairs named still suffice, and some may not be
  /// needed. Also false when it ran out before finding any layout at all, and
  /// then `conflicts` is empty.
  bool minimal = false;

  std::vector<std::pair<std::size_t, std::size_t>> conflicts;
};

/// How much deterministic time the explanation may take.
constexpr double EXPLAIN_BUDGET = 60.0;

/// The same Claims with the no-overlap made soft: a boolean per co-visible
/// pair and Phase, true iff the two overlap there, and as few of them true as
/// possible. Never infeasible, and the pairs it answers with are the smallest
/// set of collisions any layout has to accept — the one thing to fix. Asked
/// only after `solve` has found nothing; see docs/spec/diagnostics.md on
/// explaining an infeasible layout.
Explanation explain( Problem const& problem );
Explanation explain( std::span<Claim const> claims );

} // namespace nga::model
