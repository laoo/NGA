#include "nga/model/Solver.hpp"

#include "ortools/sat/cp_model.h"
#include "ortools/sat/cp_model_solver.h"
#include "ortools/sat/sat_parameters.pb.h"
#include "ortools/util/sorted_interval_list.h"

#include <algorithm>
#include <cstddef>
#include <map>

namespace nga::model
{

namespace
{

namespace sat = operations_research::sat;

/// The part of the model every question shares: a start variable per Claim
/// over its allowed starts — one per Phase for a movable Claim — and an
/// interval of its size at each; a Bank variable per Pane, and for a Claim
/// in a Pane its offset within the Window, which is the coordinate its Bank
/// is shared in.
struct Built
{
  sat::CpModelBuilder model;
  std::size_t phaseCount = 0;

  /// Per Claim that is not movable: its start, or nothing for a Claim the
  /// model does not hold — one with no allowed start, or one of no bytes.
  std::vector<std::optional<sat::IntVar>> single;

  /// Per movable Claim, per Phase: its start there, or nothing outside its
  /// Residency.
  std::vector<std::vector<std::optional<sat::IntVar>>> perPhase;

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

  /// Whether two Claims can collide at all in one address space: one group,
  /// and neither in a Pane — a Pane's Claims are held apart by Bank and
  /// offset, which is the other kind of collision.
  [[nodiscard]] static bool coVisible( Claim const& a, Claim const& b )
  {
    return a.group == b.group && !a.pane.has_value() && !b.pane.has_value();
  }

  /// Answered without the model: the first allowed start of a Claim of no
  /// bytes, which takes nothing from anyone.
  std::vector<std::optional<std::uint32_t>> settled;

  /// The start a Claim has in a Phase, whichever kind it is.
  [[nodiscard]] std::optional<sat::IntVar> startIn( std::size_t claim, std::size_t phase ) const
  {
    if ( !perPhase[claim].empty() )
    {
      return phase < perPhase[claim].size() ? perPhase[claim][phase] : std::nullopt;
    }
    return single[claim];
  }

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

/// Whether a Claim gets a start per Phase: movable, and in some Phase to
/// begin with — a Module in no Phase is placed once, like any other.
bool perPhase( Claim const& claim )
{
  return claim.movable && !claim.residency.isEmpty();
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
  built.perPhase.resize( claims.size() );
  built.settled.resize( claims.size() );

  for ( Claim const& claim : claims )
  {
    built.phaseCount = std::max( built.phaseCount, claim.residency.phaseCount() );
  }
  built.byPhase.resize( built.phaseCount );
  built.claimsByPhase.resize( built.phaseCount );
  built.offsetByPhase.resize( built.phaseCount );

  // A Bank per Pane, over the set's Banks that leave room for a family's
  // run; pinned by number where the Pane was.
  built.paneBank.resize( problem.panes.size() );
  built.paneRow.resize( problem.panes.size() );
  for ( std::size_t index = 0; index < problem.panes.size(); ++index )
  {
    PaneClaim const& pane = problem.panes[index];
    if ( pane.count == 0 || pane.banks < pane.count ||
         ( pane.bank.has_value() && *pane.bank + pane.count > pane.banks ) )
    {
      built.paneWithoutBank = true;
      continue;
    }
    std::int64_t const last = pane.banks - pane.count;
    sat::IntVar const bank = pane.bank.has_value() ? built.model.NewIntVar( { *pane.bank, *pane.bank } )
                                                   : built.model.NewIntVar( { 0, last } );
    built.paneBank[index] = bank;
    built.paneRow[index] = built.model.NewFixedSizeIntervalVar( bank, pane.count );
    if ( !pane.bank.has_value() )
    {
      built.banksInOrder.push_back( bank );
    }
  }
  for ( Occupied const& occupied : problem.occupied )
  {
    built.taken.push_back( Built::Taken{
        .x = built.model.NewFixedSizeIntervalVar( static_cast<std::int64_t>( occupied.offset ), occupied.size ),
        .y = built.model.NewFixedSizeIntervalVar( static_cast<std::int64_t>( occupied.bank ), 1 ) } );
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
    // it asks to be aligned. A boundary means nothing to it.
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

    // The interval a Phase holds apart: at the address for a Claim of the
    // address space, and at the offset within the Window for one in a Pane,
    // whose Bank is the other coordinate.
    auto const present = [&]( std::size_t phase, sat::IntVar start )
    {
      std::optional<sat::LinearExpr> offset;
      if ( pane != nullptr )
      {
        offset = offsetOf( built, start, claim.size, *pane );
      }
      built.byPhase[phase].push_back( built.model.NewFixedSizeIntervalVar( offset.value_or( start ), claim.size ) );
      built.claimsByPhase[phase].push_back( index );
      built.offsetByPhase[phase].push_back( offset );
    };

    if ( !perPhase( claim ) )
    {
      sat::IntVar const start = newStart();
      std::optional<sat::LinearExpr> const offset =
          pane != nullptr ? std::optional<sat::LinearExpr>{ offsetOf( built, start, claim.size, *pane ) }
                          : std::nullopt;
      sat::IntervalVar const interval = built.model.NewFixedSizeIntervalVar( offset.value_or( start ), claim.size );
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
      continue;
    }

    // A start per Phase, and the Claim's intervals are each in one Phase.
    built.perPhase[index].resize( built.phaseCount );
    for ( std::size_t phase = 0; phase < claim.residency.phaseCount(); ++phase )
    {
      if ( !claim.residency.includes( PhaseIndex{ static_cast<std::uint32_t>( phase ) } ) )
      {
        continue;
      }
      sat::IntVar const start = newStart();
      present( phase, start );
      built.perPhase[index][phase] = start;
    }

    // Held still wherever a Reference sees it: one start across every Phase
    // of each Residency that refers to it.
    for ( Residency const& across : claim.frozenOver )
    {
      std::optional<sat::IntVar> first;
      for ( std::size_t phase = 0; phase < std::min( across.phaseCount(), built.phaseCount ); ++phase )
      {
        std::optional<sat::IntVar> const start = built.perPhase[index][phase];
        if ( !across.includes( PhaseIndex{ static_cast<std::uint32_t>( phase ) } ) || !start.has_value() )
        {
          continue;
        }
        if ( first.has_value() )
        {
          built.model.AddEquality( *first, *start );
        }
        else
        {
          first = start;
        }
      }
    }
  }
  // Adjacency, once every start exists: a Claim that follows another starts
  // where that one ends, in every Phase both have a start. A Claim of no
  // bytes was settled without a variable, and what follows it is held to
  // the settled address.
  for ( std::size_t index = 0; index < claims.size(); ++index )
  {
    if ( !claims[index].follows.has_value() )
    {
      continue;
    }
    std::size_t const before = *claims[index].follows;
    bool const bothSingle = built.perPhase[index].empty() && built.perPhase[before].empty();
    for ( std::size_t phase = 0; phase < std::max<std::size_t>( built.phaseCount, 1 ); ++phase )
    {
      std::optional<sat::IntVar> const start = built.startIn( index, phase );
      if ( !start.has_value() )
      {
        continue;
      }
      if ( std::optional<sat::IntVar> const previous = built.startIn( before, phase ); previous.has_value() )
      {
        built.model.AddEquality( *start, *previous + claims[before].size );
      }
      else if ( built.settled[before].has_value() )
      {
        built.model.AddEquality( *start, static_cast<std::int64_t>( *built.settled[before] ) + claims[before].size );
      }
      if ( bothSingle )
      {
        break;
      }
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
      continue;
    }
    if ( built.perPhase[index].empty() )
    {
      continue;
    }
    into.byPhase[index].assign( built.phaseCount, std::nullopt );
    std::optional<std::uint32_t> one;
    bool uniform = true;
    for ( std::size_t phase = 0; phase < built.phaseCount; ++phase )
    {
      std::optional<sat::IntVar> const& start = built.perPhase[index][phase];
      if ( !start.has_value() )
      {
        continue;
      }
      auto const address = static_cast<std::uint32_t>( sat::SolutionIntegerValue( response, *start ) );
      into.byPhase[index][phase] = address;
      uniform = uniform && ( !one.has_value() || *one == address );
      one = address;
    }
    if ( uniform )
    {
      into.addresses[index] = one;
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
  return occupied.live.phaseCount() == 0 ||
         ( occupied.live.phaseCount() > phase &&
           occupied.live.includes( PhaseIndex{ static_cast<std::uint32_t>( phase ) } ) );
}

/// Whether two Claims in Panes can collide at all: Panes of one set.
bool sameSet( Problem const& problem, Claim const& a, Claim const& b )
{
  return a.pane.has_value() && b.pane.has_value() && *a.pane < problem.panes.size() && *b.pane < problem.panes.size() &&
         problem.panes[*a.pane].set == problem.panes[*b.pane].set;
}

} // namespace

Solution solve( Problem const& problem, PhaseGraph const& phases )
{
  std::span<Claim const> const claims = problem.claims;
  Built built = build( problem );
  Solution solution;
  solution.addresses = built.settled;
  solution.byPhase.resize( claims.size() );
  solution.paneBanks.resize( problem.panes.size() );
  for ( std::size_t index = 0; index < claims.size(); ++index )
  {
    if ( perPhase( claims[index] ) && built.settled[index].has_value() )
    {
      solution.byPhase[index].assign( built.phaseCount, std::nullopt );
      for ( std::size_t phase = 0; phase < claims[index].residency.phaseCount(); ++phase )
      {
        if ( claims[index].residency.includes( PhaseIndex{ static_cast<std::uint32_t>( phase ) } ) )
        {
          solution.byPhase[index][phase] = built.settled[index];
        }
      }
    }
  }

  // A Pane with no Bank to have is a layout that does not exist, and the
  // arithmetic above the solver has already said which Pane.
  if ( built.paneWithoutBank )
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
      else if ( !pairwise[present[k]] )
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

  // A move: a movable Claim standing at two addresses across an edge, which
  // at run time is its bytes copied again. One boolean per edge and Claim,
  // weighed by the bytes, and decided before any start so that the search
  // tries "no move" first.
  std::vector<sat::BoolVar> moves;
  std::vector<std::int64_t> weights;
  for ( std::uint32_t from = 0; from < phases.phases.size(); ++from )
  {
    for ( PhaseIndex const to : phases.phases[from].then )
    {
      for ( std::size_t index = 0; index < claims.size(); ++index )
      {
        std::optional<sat::IntVar> const before = built.startIn( index, from );
        std::optional<sat::IntVar> const after = built.startIn( index, to.value );
        if ( built.perPhase[index].empty() || !before.has_value() || !after.has_value() )
        {
          continue;
        }
        sat::BoolVar const move = built.model.NewBoolVar();
        built.model.AddNotEqual( *before, *after ).OnlyEnforceIf( move );
        built.model.AddEquality( *before, *after ).OnlyEnforceIf( move.Not() );
        moves.push_back( move );
        weights.push_back( claims[index].size );
      }
    }
  }

  if ( !moves.empty() )
  {
    built.model.AddDecisionStrategy(
        moves, sat::DecisionStrategyProto::CHOOSE_FIRST, sat::DecisionStrategyProto::SELECT_MIN_VALUE );
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

  // One worker and the fixed search above, and never a limit on the clock:
  // more workers would make the layout a fact about the machine, and a clock
  // would make "no layout" one.
  sat::SatParameters parameters;
  parameters.set_num_workers( 1 );
  parameters.set_search_branching( sat::SatParameters::FIXED_SEARCH );

  // Whether a layout exists is answered first and without any budget, so
  // that the answer never depends on one.
  sat::CpSolverResponse found = sat::SolveWithParameters( built.model.Build(), parameters );
  if ( !hasSolution( found ) )
  {
    solution.feasible = false;
    return solution;
  }

  // Then, where there is a choice, the layout moving the fewest bytes, and
  // among those the one whose Panes stand highest — every byte moved
  // outweighing every Bank, so that tidiness never costs a copy — from the
  // one found, within a budget, and keeping the one found where the budget
  // runs out before a better one is proven.
  if ( !moves.empty() || !built.banksInOrder.empty() )
  {
    for ( sat::IntVar const& start : built.inOrder )
    {
      built.model.AddHint( start, sat::SolutionIntegerValue( found, start ) );
    }
    for ( sat::IntVar const& bank : built.banksInOrder )
    {
      built.model.AddHint( bank, sat::SolutionIntegerValue( found, bank ) );
    }
    std::int64_t banksAtMost = 1;
    for ( PaneClaim const& pane : problem.panes )
    {
      banksAtMost += pane.banks;
    }
    sat::LinearExpr objective;
    for ( std::size_t index = 0; index < moves.size(); ++index )
    {
      objective += sat::LinearExpr::Term( moves[index], weights[index] * banksAtMost );
    }
    for ( sat::IntVar const& bank : built.banksInOrder )
    {
      objective += sat::LinearExpr::Term( bank, -1 );
    }
    built.model.Minimize( objective );
    parameters.set_max_deterministic_time( MOVE_BUDGET );
    sat::CpSolverResponse const better = sat::SolveWithParameters( built.model.Build(), parameters );
    if ( hasSolution( better ) )
    {
      found = better;
    }
  }

  readBack( built, claims, found, solution );
  return solution;
}

Solution solve( std::span<Claim const> claims, PhaseGraph const& phases )
{
  return solve( Problem{ .claims = { claims.begin(), claims.end() }, .panes = {}, .occupied = {} }, phases );
}

Explanation explain( Problem const& problem, PhaseGraph const& /*phases*/ )
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
      std::optional<sat::IntVar> const startOfA = built.startIn( a, phase );
      if ( !startOfA.has_value() || !claims[a].residency.includes( PhaseIndex{ static_cast<std::uint32_t>( phase ) } ) )
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
      for ( std::size_t b = a + 1; b < claims.size(); ++b )
      {
        std::optional<sat::IntVar> const startOfB = built.startIn( b, phase );
        if ( !startOfB.has_value() ||
             !claims[b].residency.includes( PhaseIndex{ static_cast<std::uint32_t>( phase ) } ) ||
             std::ranges::find( claims[a].sharesWith, b ) != claims[a].sharesWith.end() )
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

Explanation explain( std::span<Claim const> claims, PhaseGraph const& phases )
{
  return explain( Problem{ .claims = { claims.begin(), claims.end() }, .panes = {}, .occupied = {} }, phases );
}

} // namespace nga::model
