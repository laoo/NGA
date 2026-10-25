#include "nga/model/Solver.hpp"

#include "ortools/sat/cp_model.h"
#include "ortools/sat/cp_model_solver.h"
#include "ortools/sat/sat_parameters.pb.h"
#include "ortools/util/sorted_interval_list.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <cstddef>
#include <map>
#include <string_view>
#include <utility>

namespace nga::model
{

namespace
{

namespace sat = operations_research::sat;

/// What one question took, said at debug level: the clock, and the
/// deterministic time against the budget it had, which is what says how near
/// a program stands to running out — see
/// docs/decisions/0255-the-portfolio-has-a-budget-too.md.
void logTook( std::string_view question, sat::CpSolverResponse const& response, double budget )
{
  spdlog::debug( "Place, {}: {:.1f} ms, {:.4f} of {} deterministic time",
                 question,
                 response.wall_time() * 1000.0,
                 response.deterministic_time(),
                 budget );
}

/// One worker and the decision strategies the model holds, within a budget
/// of deterministic time and never a limit on the clock: more workers would
/// make the layout a fact about the machine, and a clock would make running
/// out one.
sat::SatParameters fixedSearch( double budget )
{
  sat::SatParameters parameters;
  parameters.set_num_workers( 1 );
  parameters.set_search_branching( sat::SatParameters::FIXED_SEARCH );
  parameters.set_max_deterministic_time( budget );
  return parameters;
}

/// The part of the model every question shares: a start variable per Claim
/// over its allowed starts, and an interval of its size; a Bank variable per Pane, and for a Claim
/// in a Pane its offset within the Window, which is the coordinate its Bank
/// is shared in.
struct Built
{
  sat::CpModelBuilder model;
  std::size_t phaseCount = 0;

  /// Per Claim: its start, or nothing for a Claim the model does not hold —
  /// one with no allowed start, or one of no bytes.
  std::vector<std::optional<sat::IntVar>> single;

  /// Every start in the order the Claims were given, for the search.
  std::vector<sat::IntVar> inOrder;

  /// Per Phase, the intervals of the Claims present in it, which Claim each
  /// is, and for a Claim in a Pane the offset the interval starts at within
  /// the Window.
  std::vector<std::vector<sat::IntervalVar>> byPhase;
  std::vector<std::vector<std::size_t>> claimsByPhase;
  std::vector<std::vector<std::optional<sat::LinearExpr>>> offsetByPhase;

  /// Per Pane: its Bank, and the run of Banks it takes as an interval —
  /// nothing for a Pane that cannot have one, which makes the whole
  /// infeasible.
  std::vector<std::optional<sat::IntVar>> paneBank;
  std::vector<std::optional<sat::IntervalVar>> paneRow;
  std::vector<sat::IntVar> banksInOrder;
  bool paneWithoutBank = false;

  /// Per occupied range: its bytes as a rectangle of the set's coordinates.
  struct Taken
  {
    sat::IntervalVar x;
    sat::IntervalVar y;
  };

  std::vector<Taken> taken;

  /// Per Claim of a unit set some Claim stands in itself: the set, and where
  /// its bytes start on the set's axis with the interval they take there —
  /// once, or once per member of a family. Nothing and empty for every other
  /// Claim. Such a set is held apart on its axis as well as by rectangles,
  /// since a Claim in the set itself may run across Banks and is no
  /// rectangle; a set with no such Claim keeps the rectangles alone, and so
  /// the layout it always had — see
  /// docs/decisions/0254-a-section-may-stand-in-a-unit-set.md.
  std::vector<std::optional<std::uint32_t>> setOf;
  std::vector<std::vector<sat::LinearExpr>> axisStarts;
  std::vector<std::vector<sat::IntervalVar>> onAxis;

  /// Per occupied range of a set held apart on its axis: its bytes there.
  std::vector<std::optional<sat::IntervalVar>> takenOnAxis;

  /// Whether two Claims can collide at all in one address space: one group,
  /// and neither of a unit set — a Pane's Claims are held apart by Bank and
  /// offset, and a Claim in a set itself on the set's axis, which are the
  /// other kinds of collision.
  [[nodiscard]] static bool coVisible( Claim const& a, Claim const& b )
  {
    return a.group == b.group && !a.pane.has_value() && !b.pane.has_value() && !a.linear.has_value() &&
           !b.linear.has_value();
  }

  /// Answered without the model: the first allowed start of a Claim of no
  /// bytes, which takes nothing from anyone.
  std::vector<std::optional<std::uint32_t>> settled;

  /// The offset a Claim in a Pane has in a Phase it is present in.
  [[nodiscard]] std::optional<sat::LinearExpr> offsetIn( std::size_t claim, std::size_t phase ) const
  {
    if ( phase >= claimsByPhase.size() )
    {
      return std::nullopt;
    }
    for ( std::size_t k = 0; k < claimsByPhase[phase].size(); ++k )
    {
      if ( claimsByPhase[phase][k] == claim )
      {
        return offsetByPhase[phase][k];
      }
    }
    return std::nullopt;
  }
};

/// The size of a Window's ranges laid end to end, which is what one Bank of
/// the set it shows holds.
std::int64_t windowSizeOf( PaneClaim const& pane )
{
  std::int64_t total = 0;
  for ( AddressRange const& range : pane.window )
  {
    total += range.size();
  }
  return total;
}

/// What one Bank of a set holds: its axis says, and where it does not, the
/// Window of a Pane of the set, which is the size offsets are counted in.
std::int64_t bankSizeOf( Problem const& problem, std::uint32_t set )
{
  if ( set < problem.axes.size() && problem.axes[set].bankSize > 0 )
  {
    return problem.axes[set].bankSize;
  }
  for ( PaneClaim const& pane : problem.panes )
  {
    if ( pane.set == set )
    {
      return windowSizeOf( pane );
    }
  }
  return 0;
}

/// Where a set's axis starts: its first Bank's linear address, or zero.
std::int64_t axisBeginOf( Problem const& problem, std::uint32_t set )
{
  return set < problem.axes.size() ? problem.axes[set].begin : 0;
}

/// A start's distance into the Window's ranges laid end to end: with one
/// range an expression, with several a variable that is the distance into
/// whichever range holds the bytes — which the allowed starts keep to one.
sat::LinearExpr offsetOf( Built& built, sat::IntVar start, std::uint32_t size, PaneClaim const& pane )
{
  if ( pane.window.size() == 1 )
  {
    return start - static_cast<std::int64_t>( pane.window.front().begin );
  }
  std::int64_t total = 0;
  for ( AddressRange const& range : pane.window )
  {
    total += range.size();
  }
  sat::IntVar const offset = built.model.NewIntVar( { 0, std::max<std::int64_t>( total - size, 0 ) } );
  std::vector<sat::BoolVar> within;
  std::int64_t prefix = 0;
  for ( AddressRange const& range : pane.window )
  {
    if ( range.size() >= size )
    {
      sat::BoolVar const here = built.model.NewBoolVar();
      built.model.AddGreaterOrEqual( start, static_cast<std::int64_t>( range.begin ) ).OnlyEnforceIf( here );
      built.model.AddLessOrEqual( start + size, static_cast<std::int64_t>( range.end ) ).OnlyEnforceIf( here );
      built.model.AddEquality( offset, start - static_cast<std::int64_t>( range.begin ) + prefix )
          .OnlyEnforceIf( here );
      within.push_back( here );
    }
    prefix += range.size();
  }
  built.model.AddExactlyOne( within );
  return offset;
}

Built build( Problem const& problem )
{
  std::span<Claim const> const claims = problem.claims;
  Built built;
  built.single.resize( claims.size() );
  built.settled.resize( claims.size() );

  for ( Claim const& claim : claims )
  {
    built.phaseCount = std::max( built.phaseCount, claim.residency.phaseCount() );
  }
  built.byPhase.resize( built.phaseCount );
  built.claimsByPhase.resize( built.phaseCount );
  built.offsetByPhase.resize( built.phaseCount );
  built.setOf.resize( claims.size() );
  built.axisStarts.resize( claims.size() );
  built.onAxis.resize( claims.size() );

  // The sets some Claim stands in itself, which are held apart on an axis.
  std::vector<std::uint32_t> axisSets;
  for ( Claim const& claim : claims )
  {
    if ( claim.linear.has_value() && std::ranges::find( axisSets, *claim.linear ) == axisSets.end() )
    {
      axisSets.push_back( *claim.linear );
    }
  }
  auto const onAnAxis = [&axisSets]( std::uint32_t set )
  { return std::ranges::find( axisSets, set ) != axisSets.end(); };

  // A Bank per Pane, over the set's Banks that leave room for a family's
  // run.
  built.paneBank.resize( problem.panes.size() );
  built.paneRow.resize( problem.panes.size() );
  for ( std::size_t index = 0; index < problem.panes.size(); ++index )
  {
    PaneClaim const& pane = problem.panes[index];
    if ( pane.count == 0 || pane.banks < pane.count )
    {
      built.paneWithoutBank = true;
      continue;
    }
    std::int64_t const last = pane.banks - pane.count;
    sat::IntVar const bank = built.model.NewIntVar( { 0, last } );
    built.paneBank[index] = bank;
    built.paneRow[index] = built.model.NewFixedSizeIntervalVar( bank, pane.count );
    built.banksInOrder.push_back( bank );
  }
  for ( Occupied const& occupied : problem.occupied )
  {
    built.taken.push_back( Built::Taken{
        .x = built.model.NewFixedSizeIntervalVar( static_cast<std::int64_t>( occupied.offset ), occupied.size ),
        .y = built.model.NewFixedSizeIntervalVar( static_cast<std::int64_t>( occupied.bank ), 1 ) } );

    // On the axis, only what lies within a Bank: past that no Claim of the
    // set stands, and the rectangle never reached the next Bank either.
    std::int64_t const bankSize = bankSizeOf( problem, occupied.set );
    if ( !onAnAxis( occupied.set ) || bankSize == 0 || std::cmp_greater_equal( occupied.offset, bankSize ) )
    {
      built.takenOnAxis.emplace_back( std::nullopt );
      continue;
    }
    std::int64_t const at = axisBeginOf( problem, occupied.set ) + ( std::int64_t{ occupied.bank } * bankSize ) +
                            static_cast<std::int64_t>( occupied.offset );
    built.takenOnAxis.emplace_back( built.model.NewFixedSizeIntervalVar(
        at, std::min<std::int64_t>( occupied.size, bankSize - occupied.offset ) ) );
  }

  for ( std::size_t index = 0; index < claims.size(); ++index )
  {
    Claim const& claim = claims[index];

    // The starts as closed intervals, which is how a domain is spelled; a
    // range with nothing in it is not one.
    std::vector<operations_research::ClosedInterval> intervals;
    intervals.reserve( claim.allowedStarts.size() );
    for ( AddressRange const& range : claim.allowedStarts )
    {
      if ( range.begin < range.end )
      {
        intervals.emplace_back( range.begin, static_cast<std::int64_t>( range.end ) - 1 );
      }
    }

    // Nothing large enough anywhere: unplaced by arithmetic, and kept out of
    // the model, where an empty domain would make the whole of it infeasible
    // and say nothing about which Claim did that.
    if ( intervals.empty() )
    {
      continue;
    }

    // A Section of no bytes takes nothing from anyone, and the first start
    // its Constraints allow is as good as any — the first aligned one, where
    // it asks to be aligned. A boundary means nothing to it. One that follows
    // another stands where that one ends, wherever it does: a start of its
    // own, which the adjacency below ties, and no interval, since there is
    // nothing of it to hold apart — up to the end of what its starts allow,
    // which is where a Proc ending its pool leaves it.
    if ( claim.size == 0 && claim.follows.has_value() )
    {
      built.single[index] = built.model.NewIntVar( { intervals.front().start, intervals.back().end + 1 } );
      continue;
    }
    if ( claim.size == 0 )
    {
      for ( operations_research::ClosedInterval const& interval : intervals )
      {
        std::int64_t const aligned = ( interval.start + claim.alignment - 1 ) / claim.alignment * claim.alignment;
        if ( aligned <= interval.end )
        {
          built.settled[index] = static_cast<std::uint32_t>( aligned );
          break;
        }
      }
      continue;
    }

    // A Claim in a Pane that has no Bank has no place either.
    PaneClaim const* pane = nullptr;
    if ( claim.pane.has_value() )
    {
      if ( *claim.pane >= problem.panes.size() || !built.paneBank[*claim.pane].has_value() )
      {
        continue;
      }
      pane = &problem.panes[*claim.pane];
    }

    operations_research::Domain const domain = operations_research::Domain::FromIntervals( intervals );

    // One start over the domain, with the Claim's stride and boundary on it.
    // Alignment is a linear equation with one more variable, where the same
    // set of starts spelled as a domain would be one interval per multiple;
    // a boundary is a multiple of it plus an offset that leaves room for the
    // bytes before the next one.
    auto const newStart = [&] -> sat::IntVar
    {
      sat::IntVar const start = built.model.NewIntVar( domain );
      if ( claim.alignment > 1 )
      {
        std::int64_t const alignment = claim.alignment;
        sat::IntVar const multiple = built.model.NewIntVar( { domain.Min() / alignment, domain.Max() / alignment } );
        built.model.AddEquality( start, sat::LinearExpr::Term( multiple, alignment ) );
      }
      if ( claim.boundary > 0 && claim.size <= claim.boundary )
      {
        std::int64_t const boundary = claim.boundary;
        sat::IntVar const block = built.model.NewIntVar( { domain.Min() / boundary, domain.Max() / boundary } );
        sat::IntVar const offset = built.model.NewIntVar( { 0, boundary - claim.size } );
        built.model.AddEquality( start, sat::LinearExpr::Term( block, boundary ) + offset );
      }
      built.inOrder.push_back( start );
      return start;
    };

    // The interval each Phase holds apart: at the address for a Claim of the
    // address space, and at the offset within the Window for one in a Pane,
    // whose Bank is the other coordinate.
    sat::IntVar const start = newStart();
    std::optional<sat::LinearExpr> const offset =
        pane != nullptr ? std::optional<sat::LinearExpr>{ offsetOf( built, start, claim.size, *pane ) } : std::nullopt;
    sat::IntervalVar const interval = built.model.NewFixedSizeIntervalVar( offset.value_or( start ), claim.size );

    // On the set's axis where the set has one: a Claim in the set itself at
    // its start, which is a linear address already, and a Pane's at its
    // Bank's start plus its offset, once per member of a family — in a
    // variable of its own, since an interval starts at an expression of one
    // variable and this one has the Bank's and the offset's.
    if ( claim.linear.has_value() )
    {
      built.setOf[index] = claim.linear;
      built.axisStarts[index].emplace_back( start );
      built.onAxis[index].push_back( interval );
    }
    else if ( pane != nullptr && onAnAxis( pane->set ) )
    {
      std::int64_t const bankSize = windowSizeOf( *pane );
      std::int64_t const begin = axisBeginOf( problem, pane->set );
      for ( std::uint32_t member = 0; member < pane->count; ++member )
      {
        sat::IntVar const at = built.model.NewIntVar( { begin, begin + ( std::int64_t{ pane->banks } * bankSize ) } );
        built.model.AddEquality( at,
                                 sat::LinearExpr::Term( *built.paneBank[*claim.pane], bankSize ) + *offset +
                                     ( begin + ( member * bankSize ) ) );
        built.axisStarts[index].emplace_back( at );
        built.onAxis[index].push_back( built.model.NewFixedSizeIntervalVar( at, claim.size ) );
      }
      built.setOf[index] = pane->set;
    }
    for ( std::size_t phase = 0; phase < claim.residency.phaseCount(); ++phase )
    {
      if ( claim.residency.includes( PhaseIndex{ static_cast<std::uint32_t>( phase ) } ) )
      {
        built.byPhase[phase].push_back( interval );
        built.claimsByPhase[phase].push_back( index );
        built.offsetByPhase[phase].push_back( offset );
      }
    }
    built.single[index] = start;
  }
  // Adjacency, once every start exists: a Claim that follows another starts
  // where that one ends. A Claim of no bytes that follows nothing was settled
  // without a variable, and what follows it is held to the settled address.
  for ( std::size_t index = 0; index < claims.size(); ++index )
  {
    std::optional<sat::IntVar> const& start = built.single[index];
    std::optional<std::size_t> const& follows = claims[index].follows;
    if ( !follows.has_value() || !start.has_value() )
    {
      continue;
    }
    std::size_t const before = follows.value();
    if ( std::optional<sat::IntVar> const& previous = built.single[before]; previous.has_value() )
    {
      built.model.AddEquality( start.value(), previous.value() + claims[before].size );
    }
    else if ( std::optional<std::uint32_t> const& settled = built.settled[before]; settled.has_value() )
    {
      built.model.AddEquality( start.value(), static_cast<std::int64_t>( settled.value() ) + claims[before].size );
    }
  }
  return built;
}

bool hasSolution( sat::CpSolverResponse const& response )
{
  return response.status() == sat::CpSolverStatus::OPTIMAL || response.status() == sat::CpSolverStatus::FEASIBLE;
}

/// What the solver decided, read back into the answer's shape.
void readBack( Built const& built,
               std::span<Claim const> claims,
               sat::CpSolverResponse const& response,
               Solution& into )
{
  for ( std::size_t index = 0; index < claims.size(); ++index )
  {
    if ( std::optional<sat::IntVar> const& start = built.single[index]; start.has_value() )
    {
      into.addresses[index] = static_cast<std::uint32_t>( sat::SolutionIntegerValue( response, *start ) );
    }
  }
  for ( std::size_t index = 0; index < built.paneBank.size(); ++index )
  {
    if ( std::optional<sat::IntVar> const& bank = built.paneBank[index]; bank.has_value() )
    {
      into.paneBanks[index] = static_cast<std::uint32_t>( sat::SolutionIntegerValue( response, *bank ) );
    }
  }
}

/// A boolean that is true iff two ranges of one axis overlap, reified both
/// ways: the model must be free to overlap them, and must not be free to
/// call apart a pair that is not.
sat::BoolVar overlapOf( sat::CpModelBuilder& model,
                        sat::LinearExpr const& startOfA,
                        std::int64_t sizeOfA,
                        sat::LinearExpr const& startOfB,
                        std::int64_t sizeOfB )
{
  sat::BoolVar const aFirst = model.NewBoolVar();
  model.AddLessOrEqual( startOfA + sizeOfA, startOfB ).OnlyEnforceIf( aFirst );
  model.AddGreaterThan( startOfA + sizeOfA, startOfB ).OnlyEnforceIf( aFirst.Not() );

  sat::BoolVar const bFirst = model.NewBoolVar();
  model.AddLessOrEqual( startOfB + sizeOfB, startOfA ).OnlyEnforceIf( bFirst );
  model.AddGreaterThan( startOfB + sizeOfB, startOfA ).OnlyEnforceIf( bFirst.Not() );

  sat::BoolVar const overlap = model.NewBoolVar();
  model.AddBoolOr( { aFirst, bFirst } ).OnlyEnforceIf( overlap.Not() );
  model.AddBoolAnd( { aFirst.Not(), bFirst.Not() } ).OnlyEnforceIf( overlap );
  return overlap;
}

/// Whether taken bytes are taken in a Phase.
bool takenIn( Occupied const& occupied, std::size_t phase )
{
  return occupied.live.phaseCount() == 0 || occupied.live.includes( PhaseIndex{ static_cast<std::uint32_t>( phase ) } );
}

/// Whether two Claims in Panes can collide at all: Panes of one set.
bool sameSet( Problem const& problem, Claim const& a, Claim const& b )
{
  return a.pane.has_value() && b.pane.has_value() && *a.pane < problem.panes.size() && *b.pane < problem.panes.size() &&
         problem.panes[*a.pane].set == problem.panes[*b.pane].set;
}

} // namespace

Solution solve( Problem const& problem )
{
  std::span<Claim const> const claims = problem.claims;
  Built built = build( problem );
  Solution solution;
  solution.addresses = built.settled;
  solution.paneBanks.resize( problem.panes.size() );

  // A Pane with no Bank to have is a layout that does not exist, and the
  // arithmetic above the solver has already said which Pane.
  if ( built.paneWithoutBank || problem.refused )
  {
    solution.feasible = false;
    return solution;
  }

  // The co-visibility rule, once per Phase: two Claims collide iff some Phase
  // holds both, and a Phase that holds one of them is no constraint at all.
  // A Claim with a partner it may overlap cannot be in the no-overlap, which
  // excepts no pair; it is held apart from every other Claim of the Phase
  // pair by pair, except the partners it names.
  std::vector<bool> pairwise( claims.size(), false );
  for ( std::size_t index = 0; index < claims.size(); ++index )
  {
    pairwise[index] = !claims[index].sharesWith.empty();
  }
  for ( std::size_t phase = 0; phase < built.phaseCount; ++phase )
  {
    // One no-overlap per group: a Window shows one state at a time, so
    // Claims in different states never collide. A Claim in a Pane is a
    // rectangle instead — its offset within the Window by the Bank its Pane
    // has — in one two-dimensional no-overlap per set, with the bytes
    // already taken there: two of them collide iff their Banks and their
    // offsets both meet, which is docs/decisions/0054-panes.md's rule with
    // the Bank left to the solver.
    std::map<std::optional<std::uint32_t>, std::vector<sat::IntervalVar>> apart;
    std::map<std::uint32_t, std::vector<std::pair<sat::IntervalVar, sat::IntervalVar>>> rectangles;
    std::vector<std::size_t> const& present = built.claimsByPhase[phase];
    for ( std::size_t k = 0; k < present.size(); ++k )
    {
      Claim const& claim = claims[present[k]];
      if ( claim.pane.has_value() )
      {
        // Present only with a Bank to be in: build() held back a Claim of a
        // Pane that has none.
        if ( std::optional<sat::IntervalVar> const& row = built.paneRow[*claim.pane]; row.has_value() )
        {
          rectangles[problem.panes[*claim.pane].set].emplace_back( built.byPhase[phase][k], *row );
        }
      }
      else if ( !claim.linear.has_value() && !pairwise[present[k]] )
      {
        apart[claim.group].push_back( built.byPhase[phase][k] );
      }
    }
    for ( auto const& [group, intervals] : apart )
    {
      if ( intervals.size() > 1 )
      {
        built.model.AddNoOverlap( intervals );
      }
    }
    for ( auto& [set, ofSet] : rectangles )
    {
      for ( std::size_t index = 0; index < problem.occupied.size(); ++index )
      {
        if ( problem.occupied[index].set == set && takenIn( problem.occupied[index], phase ) )
        {
          ofSet.emplace_back( built.taken[index].x, built.taken[index].y );
        }
      }
      if ( ofSet.size() < 2 )
      {
        continue;
      }
      sat::NoOverlap2DConstraint apartInSet = built.model.AddNoOverlap2D();
      for ( auto const& [x, y] : ofSet )
      {
        apartInSet.AddRectangle( x, y );
      }
    }

    // A set some Claim stands in itself is one axis as well: that Claim's
    // bytes may run across Banks, which no rectangle holds, so every Claim of
    // the set and every byte taken there is one interval on it — see
    // docs/decisions/0254-a-section-may-stand-in-a-unit-set.md.
    std::map<std::uint32_t, std::vector<sat::IntervalVar>> onAxis;
    for ( std::size_t const claim : present )
    {
      if ( std::optional<std::uint32_t> const set = built.setOf[claim]; set.has_value() )
      {
        std::vector<sat::IntervalVar>& ofSet = onAxis[*set];
        ofSet.insert( ofSet.end(), built.onAxis[claim].begin(), built.onAxis[claim].end() );
      }
    }
    for ( auto& [set, ofSet] : onAxis )
    {
      for ( std::size_t index = 0; index < problem.occupied.size(); ++index )
      {
        std::optional<sat::IntervalVar> const& taken = built.takenOnAxis[index];
        if ( problem.occupied[index].set == set && takenIn( problem.occupied[index], phase ) && taken.has_value() )
        {
          ofSet.push_back( *taken );
        }
      }
      if ( ofSet.size() > 1 )
      {
        built.model.AddNoOverlap( ofSet );
      }
    }
    for ( std::size_t ka = 0; ka < present.size(); ++ka )
    {
      std::size_t const a = present[ka];
      if ( !pairwise[a] )
      {
        continue;
      }
      for ( std::size_t kb = 0; kb < present.size(); ++kb )
      {
        // Each pair once: a pairwise Claim against every Claim not pairwise,
        // and against the pairwise ones after it. A no-overlap of two, and
        // not a disjunction of its own, so that the search treats the pair
        // as it treats the rest.
        std::size_t const b = present[kb];
        if ( b == a || ( pairwise[b] && b < a ) || !Built::coVisible( claims[a], claims[b] ) ||
             std::ranges::find( claims[a].sharesWith, b ) != claims[a].sharesWith.end() )
        {
          continue;
        }
        built.model.AddNoOverlap( { built.byPhase[phase][ka], built.byPhase[phase][kb] } );
      }
    }
  }

  // Highest Bank first: the Panes go to the top of the set, and storage,
  // packed from the bottom, meets them as late as it can.
  if ( !built.banksInOrder.empty() )
  {
    built.model.AddDecisionStrategy(
        built.banksInOrder, sat::DecisionStrategyProto::CHOOSE_FIRST, sat::DecisionStrategyProto::SELECT_MAX_VALUE );
  }
  if ( !built.inOrder.empty() )
  {
    built.model.AddDecisionStrategy(
        built.inOrder, sat::DecisionStrategyProto::CHOOSE_FIRST, sat::DecisionStrategyProto::SELECT_MIN_VALUE );
  }

  // Whether a layout exists is answered first: by the fixed search within
  // its budget, and where that runs out by CP-SAT's portfolio, interleaved
  // in fixed batches, within one of its own — see
  // docs/decisions/0244-the-fixed-search-has-a-budget-and-a-portfolio-after-it.md
  // and docs/decisions/0255-the-portfolio-has-a-budget-too.md.
  sat::CpSolverResponse found = sat::SolveWithParameters( built.model.Build(), fixedSearch( FIND_BUDGET ) );
  logTook( "finding a layout", found, FIND_BUDGET );
  if ( found.status() == sat::CpSolverStatus::UNKNOWN )
  {
    // With no stop at the first layout: the tasks of a batch run on threads,
    // and the first of them to find one would win by the clock. A model with
    // no objective ends with the batch that found it — see
    // docs/decisions/0257-the-portfolio-ends-with-its-batch-and-the-raise-is-the-fixed-search.md.
    sat::SatParameters portfolio;
    portfolio.set_num_workers( PORTFOLIO_WORKERS );
    portfolio.set_interleave_search( true );
    portfolio.set_max_deterministic_time( PORTFOLIO_BUDGET );
    found = sat::SolveWithParameters( built.model.Build(), portfolio );
    logTook( "the portfolio", found, PORTFOLIO_BUDGET );
  }
  if ( !hasSolution( found ) )
  {
    solution.feasible = false;
    solution.decided = found.status() != sat::CpSolverStatus::UNKNOWN;
    return solution;
  }

  // Then, where there is a choice, the layout whose Panes stand highest, from
  // the one found, within a budget, and keeping the one found where the
  // budget runs out before a better one is proven. Asked by the fixed search
  // whichever search found the layout: the same question gets the same
  // answer, and no parameter of the portfolio's is carried into it — see
  // docs/decisions/0257-the-portfolio-ends-with-its-batch-and-the-raise-is-the-fixed-search.md.
  //
  // The layout found is the hint, every variable of it and not only the ones
  // the search decides: CP-SAT searches for a hint short of a single variable
  // again before it starts on the proof, and a whole one it only checks, which
  // also hands presolve the objective to beat. On the Kung Fu Master port that
  // took the question from a quarter of a second to a sixteenth, and no layout
  // of the suite moved.
  if ( !built.banksInOrder.empty() )
  {
    int const variables = built.model.Proto().variables_size();
    for ( int index = 0; index < variables; ++index )
    {
      built.model.AddHint( built.model.GetIntVarFromProtoIndex( index ), found.solution( index ) );
    }
    sat::LinearExpr objective;
    for ( sat::IntVar const& bank : built.banksInOrder )
    {
      objective += sat::LinearExpr::Term( bank, -1 );
    }
    built.model.Minimize( objective );
    sat::CpSolverResponse const better = sat::SolveWithParameters( built.model.Build(), fixedSearch( RAISE_BUDGET ) );
    logTook( "raising the Panes", better, RAISE_BUDGET );
    if ( hasSolution( better ) )
    {
      found = better;
    }
  }

  readBack( built, claims, found, solution );
  return solution;
}

Solution solve( std::span<Claim const> claims )
{
  return solve( Problem{ .claims = { claims.begin(), claims.end() }, .panes = {}, .occupied = {}, .axes = {} } );
}

Explanation explain( Problem const& problem )
{
  std::span<Claim const> const claims = problem.claims;
  Built built = build( problem );
  Explanation explanation;
  if ( built.paneWithoutBank )
  {
    return explanation;
  }

  // Whether two Panes' runs of Banks meet, which does not depend on the
  // Phase; asked once per pair.
  std::map<std::pair<std::size_t, std::size_t>, sat::BoolVar> banksMeet;
  auto const banksMeetOf = [&]( std::size_t a, std::size_t b )
  {
    auto const key = std::make_pair( std::min( a, b ), std::max( a, b ) );
    if ( auto const found = banksMeet.find( key ); found != banksMeet.end() )
    {
      return found->second;
    }
    sat::BoolVar const meet = overlapOf( built.model,
                                         *built.paneBank[key.first],
                                         problem.panes[key.first].count,
                                         *built.paneBank[key.second],
                                         problem.panes[key.second].count );
    banksMeet.emplace( key, meet );
    return meet;
  };

  // A boolean per co-visible pair and Phase that is true iff the two overlap
  // there: at the address for two Claims of one address space, and in both
  // Bank and offset for two in Panes of one set. The bytes a set already
  // holds are not free to be overlapped, since nothing can be done about
  // them: a Claim in a Pane is kept off them outright.
  std::vector<sat::BoolVar> overlaps;
  std::vector<std::pair<std::size_t, std::size_t>> pairs;
  for ( std::size_t phase = 0; phase < built.phaseCount; ++phase )
  {
    for ( std::size_t a = 0; a < claims.size(); ++a )
    {
      std::optional<sat::IntVar> const startOfA = built.single[a];
      if ( !startOfA.has_value() || claims[a].size == 0 ||
           !claims[a].residency.includes( PhaseIndex{ static_cast<std::uint32_t>( phase ) } ) )
      {
        continue;
      }
      std::optional<sat::LinearExpr> const offsetOfA = built.offsetIn( a, phase );
      if ( claims[a].pane.has_value() && offsetOfA.has_value() )
      {
        for ( std::size_t index = 0; index < problem.occupied.size(); ++index )
        {
          Occupied const& occupied = problem.occupied[index];
          if ( occupied.set != problem.panes[*claims[a].pane].set || !takenIn( occupied, phase ) )
          {
            continue;
          }
          sat::BoolVar const offsets =
              overlapOf( built.model, *offsetOfA, claims[a].size, occupied.offset, occupied.size );
          sat::BoolVar const banks = overlapOf(
              built.model, *built.paneBank[*claims[a].pane], problem.panes[*claims[a].pane].count, occupied.bank, 1 );
          built.model.AddBoolOr( { offsets.Not(), banks.Not() } );
        }
      }
      if ( claims[a].linear.has_value() )
      {
        for ( std::size_t index = 0; index < problem.occupied.size(); ++index )
        {
          if ( problem.occupied[index].set == *claims[a].linear && takenIn( problem.occupied[index], phase ) &&
               built.takenOnAxis[index].has_value() )
          {
            built.model.AddNoOverlap( { built.onAxis[a].front(), *built.takenOnAxis[index] } );
          }
        }
      }
      for ( std::size_t b = a + 1; b < claims.size(); ++b )
      {
        std::optional<sat::IntVar> const startOfB = built.single[b];
        if ( !startOfB.has_value() || claims[b].size == 0 ||
             !claims[b].residency.includes( PhaseIndex{ static_cast<std::uint32_t>( phase ) } ) ||
             std::ranges::find( claims[a].sharesWith, b ) != claims[a].sharesWith.end() ||
             std::ranges::find( claims[b].sharesWith, a ) != claims[b].sharesWith.end() )
        {
          continue;
        }
        std::optional<sat::BoolVar> overlap;
        if ( Built::coVisible( claims[a], claims[b] ) )
        {
          overlap = overlapOf( built.model, *startOfA, claims[a].size, *startOfB, claims[b].size );
        }
        else if ( sameSet( problem, claims[a], claims[b] ) )
        {
          std::optional<sat::LinearExpr> const offsetOfB = built.offsetIn( b, phase );
          if ( !offsetOfA.has_value() || !offsetOfB.has_value() )
          {
            continue;
          }
          sat::BoolVar const offsets = overlapOf( built.model, *offsetOfA, claims[a].size, *offsetOfB, claims[b].size );
          sat::BoolVar const both = built.model.NewBoolVar();
          sat::BoolVar const banks = banksMeetOf( *claims[a].pane, *claims[b].pane );
          built.model.AddBoolAnd( { offsets, banks } ).OnlyEnforceIf( both );
          built.model.AddBoolOr( { offsets.Not(), banks.Not() } ).OnlyEnforceIf( both.Not() );
          overlap = both;
        }
        else if ( ( claims[a].linear.has_value() || claims[b].linear.has_value() ) && built.setOf[a].has_value() &&
                  built.setOf[a] == built.setOf[b] )
        {
          // On the axis, where a family's member meeting the other is enough.
          std::vector<sat::BoolVar> meets;
          for ( sat::LinearExpr const& atA : built.axisStarts[a] )
          {
            for ( sat::LinearExpr const& atB : built.axisStarts[b] )
            {
              meets.push_back( overlapOf( built.model, atA, claims[a].size, atB, claims[b].size ) );
            }
          }
          sat::BoolVar const any = built.model.NewBoolVar();
          built.model.AddBoolOr( meets ).OnlyEnforceIf( any );
          for ( sat::BoolVar const& one : meets )
          {
            built.model.AddImplication( one, any );
          }
          overlap = any;
        }
        if ( !overlap.has_value() )
        {
          continue;
        }
        overlaps.push_back( *overlap );
        pairs.emplace_back( a, b );
      }
    }
  }
  built.model.Minimize( sat::LinearExpr::Sum( overlaps ) );

  // Minimising is the one question here that can take long, so it is bounded
  // — by deterministic time, which counts work and not the clock, so that the
  // answer on one machine is the answer on every machine. Running out is
  // reported, not hidden: the pairs found still suffice, they are just not
  // shown to be the fewest.
  sat::SatParameters parameters;
  parameters.set_num_workers( 1 );
  parameters.set_max_deterministic_time( EXPLAIN_BUDGET );

  sat::CpSolverResponse const response = sat::SolveWithParameters( built.model.Build(), parameters );
  logTook( "explaining", response, EXPLAIN_BUDGET );
  explanation.minimal = response.status() == sat::CpSolverStatus::OPTIMAL;
  if ( !hasSolution( response ) )
  {
    return explanation;
  }
  // A pair once, however many Phases it collides in.
  for ( std::size_t index = 0; index < overlaps.size(); ++index )
  {
    if ( sat::SolutionBooleanValue( response, overlaps[index] ) &&
         std::ranges::find( explanation.conflicts, pairs[index] ) == explanation.conflicts.end() )
    {
      explanation.conflicts.push_back( pairs[index] );
    }
  }
  std::ranges::sort( explanation.conflicts );
  return explanation;
}

Explanation explain( std::span<Claim const> claims )
{
  return explain( Problem{ .claims = { claims.begin(), claims.end() }, .panes = {}, .occupied = {}, .axes = {} } );
}

} // namespace nga::model
