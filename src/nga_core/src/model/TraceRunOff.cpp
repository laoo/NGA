#include "nga/model/TraceGraph.hpp"

#include "nga/model/Evaluate.hpp"
#include "nga/model/Isa.hpp"
#include "nga/model/Prune.hpp"
#include "nga/model/References.hpp"
#include "nga/model/Transition.hpp"
#include "nga/model/TypeCheck.hpp"

#include <algorithm>
#include <cstddef>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <variant>
#include <vector>

namespace nga::model::flow
{

namespace
{

/// What runs off the end of a Section, and what returns to nothing.
class RunOff
{
public:
  explicit RunOff( Graph const& graph ) : mBuild( &graph.build() ), mGraph( &graph ) {}

  /// Control that goes past the end of a Section with no `then` goes into
  /// whatever the solver placed there, so it is reported where some run can
  /// reach it: from the Section's start where that is an instruction, from
  /// every position another Section goes to, and from every instruction of
  /// one that owns an address of itself, since its own jumps may land on any.
  /// A Section of data alone is entered by nothing and says nothing — see
  /// docs/decisions/0233-control-that-runs-off-the-end-is-a-warning.md.
  void reportRunningOff( diag::DiagnosticSink& sink ) const
  {
    std::vector<bool> const returns = mayReturn();

    // A call goes on past itself only where something it calls may come
    // back; past a call to code that never returns nothing runs.
    auto const goesOn = [&]( Point const& point )
    {
      if ( point.traps )
      {
        return false;
      }
      if ( !point.calls || point.callTargets.empty() )
      {
        return true;
      }
      return std::ranges::any_of( point.callTargets, [&]( std::uint32_t target ) { return returns[target]; } );
    };

    std::vector<std::vector<bool>> reached( nodes().size() );
    std::vector<std::pair<std::uint32_t, std::uint32_t>> pending;
    auto const reach = [&]( std::uint32_t node, std::uint32_t point )
    {
      if ( point < reached[node].size() && !reached[node][point] && nodes()[node].points[point].instruction )
      {
        reached[node][point] = true;
        pending.emplace_back( node, point );
      }
    };
    for ( std::uint32_t index = 0; index < nodes().size(); ++index )
    {
      reached[index].assign( nodes()[index].points.size(), false );
    }
    for ( std::uint32_t index = 0; index < nodes().size(); ++index )
    {
      Node const& node = nodes()[index];
      for ( std::uint32_t at = 0; at < node.points.size(); ++at )
      {
        if ( at == 0 || node.ownsSelf )
        {
          reach( index, at );
        }
        for ( Landing const& landing : node.points[at].transfers )
        {
          reach( landing.node, landing.point );
        }
      }
    }
    // Within a Section control also runs into data, which is then what
    // runs off the end, so a data Point is reached by running on and by
    // nothing else.
    while ( !pending.empty() )
    {
      auto const [node, at] = pending.back();
      pending.pop_back();
      Point const& point = nodes()[node].points[at];
      if ( !goesOn( point ) )
      {
        continue;
      }
      for ( std::uint32_t const next : point.successors )
      {
        if ( next < reached[node].size() && !reached[node][next] )
        {
          reached[node][next] = true;
          pending.emplace_back( node, next );
        }
      }
    }
    for ( std::uint32_t index = 0; index < nodes().size(); ++index )
    {
      Node const& node = nodes()[index];
      for ( std::uint32_t at = 0; at < node.points.size(); ++at )
      {
        Point const& point = node.points[at];
        if ( reached[index][at] && point.runsOff && goesOn( point ) )
        {
          sink.add( diag::diagnostic( diag::DiagnosticId::RUNS_OFF_THE_END )
                        .at( point.statement.begin, point.statement.length )
                        .arg( "section", mGraph->nameOf( node ) ) );
        }
      }
    }
  }

  /// A `final` Phase keeps nothing of what only a return runs, so its entry,
  /// which the program's return would leave from, has to be one that cannot
  /// come back — see
  /// docs/decisions/0245-a-final-phase-keeps-nothing-only-an-exit-runs.md.
  void reportFinalReturns( diag::DiagnosticSink& sink ) const
  {
    PhaseGraph const& phases = mBuild->phases();
    GlobalSymbols const& symbols = mBuild->symbols();
    std::vector<bool> returns;
    for ( std::uint32_t index = 0; index < phases.phases.size(); ++index )
    {
      Phase const& phase = phases.phases[index];
      if ( !phase.finalAt.has_value() )
      {
        continue;
      }
      std::optional<SymbolRef> const entry = entrySymbolOf( phases, PhaseIndex{ index }, symbols );
      if ( !entry.has_value() )
      {
        continue;
      }
      std::optional<Position> const position = positionOf( symbols, *entry );
      if ( !position.has_value() )
      {
        continue;
      }
      if ( returns.empty() )
      {
        returns = mayReturn();
      }
      if ( returns[mGraph->indexOf( position->where )] )
      {
        sink.add( diag::diagnostic( diag::DiagnosticId::FINAL_PHASE_RETURNS )
                      .at( phase.finalAt->begin, phase.finalAt->length )
                      .arg( "phase", phase.displayName() )
                      .arg( "entry", mGraph->nameOf( nodes()[mGraph->indexOf( position->where )] ) ) );
      }
    }
  }

private:
  /// Per Section, whether a call into it may come back: it holds a return,
  /// jumps where the tool cannot follow, or jumps into a Section that may.
  /// Taken to be so wherever it cannot be shown otherwise, since what this
  /// decides is only where a finding is not made.
  [[nodiscard]] std::vector<bool> mayReturn() const
  {
    std::vector<bool> returns( nodes().size(), false );
    for ( std::uint32_t index = 0; index < nodes().size(); ++index )
    {
      Node const& node = nodes()[index];
      returns[index] =
          !node.code || node.ownsCode || node.ownsSelf ||
          std::ranges::any_of( node.points, []( Point const& point ) { return point.returns || point.loose; } );
    }
    for ( bool changed = true; changed; )
    {
      changed = false;
      for ( std::uint32_t index = 0; index < nodes().size(); ++index )
      {
        if ( returns[index] )
        {
          continue;
        }
        for ( Point const& point : nodes()[index].points )
        {
          if ( point.calls )
          {
            continue;
          }
          if ( std::ranges::any_of( point.transfers, [&]( Landing const& landing ) { return returns[landing.node]; } ) )
          {
            returns[index] = true;
            changed = true;
            break;
          }
        }
      }
    }
    return returns;
  }

  [[nodiscard]] std::vector<Node> const& nodes() const
  {
    return mGraph->nodes();
  }

  Pruned const* mBuild;
  Graph const* mGraph;
};

} // namespace

void reportRunningOff( Graph const& graph, diag::DiagnosticSink& sink )
{
  RunOff{ graph }.reportRunningOff( sink );
}

void reportFinalReturns( Graph const& graph, diag::DiagnosticSink& sink )
{
  RunOff{ graph }.reportFinalReturns( sink );
}

} // namespace nga::model::flow
