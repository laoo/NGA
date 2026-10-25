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

/// Where the Holders' flow takes what they are handed.
class HolderFlow
{
public:
  explicit HolderFlow( Graph const& graph ) : mBuild( &graph.build() ), mGraph( &graph ) {}

  /// Where every Holder may hold what: a flow forward over the Points, the
  /// Load before the entry Phase, each `.transition` carrying what it is
  /// reached with into the Phase it enters, until nothing more is carried.
  /// What the hardware's own code and data hand over has no place in the flow
  /// and counts wherever they are in memory. What a held Section names is
  /// held with it — see
  /// docs/decisions/0247-the-hardware-holds-a-section-for-as-long-as-it-is-given-it.md.
  [[nodiscard]] Holds computeHolds( std::vector<bool> const& always ) const
  {
    Holds holds;
    Project const& project = mBuild->project();
    PhaseGraph const& phases = mBuild->phases();
    GlobalSymbols const& symbols = mBuild->symbols();
    std::size_t const holderCount = mBuild->target().holders.size();
    std::size_t const phaseCount = phases.phases.size();

    // The Sections ever handed over, numbered for a state of one bit per
    // Holder and Section.
    std::vector<std::uint32_t> handed;
    std::map<std::uint32_t, std::uint32_t> slotOf;
    for ( Node const& node : nodes() )
    {
      for ( Point const& point : node.points )
      {
        for ( auto const& [holder, target] : point.handsOver )
        {
          if ( slotOf.try_emplace( target, static_cast<std::uint32_t>( handed.size() ) ).second )
          {
            handed.push_back( target );
          }
        }
      }
    }
    if ( holderCount == 0 || handed.empty() )
    {
      return holds;
    }
    std::size_t const width = handed.size();
    using State = std::vector<bool>;
    State const none( holderCount * width, false );
    auto const unite = []( State& into, State const& from )
    {
      bool changed = false;
      for ( std::size_t bit = 0; bit < from.size(); ++bit )
      {
        if ( from[bit] && !into[bit] )
        {
          into[bit] = true;
          changed = true;
        }
      }
      return changed;
    };
    auto const handOver = [&]( State& state, Point const& point )
    {
      for ( auto const& [holder, target] : point.handsOver )
      {
        state[( holder * width ) + slotOf.at( target )] = true;
      }
    };

    // What a Point leaves: a release empties its Holder, and a handing over
    // replaces what its Holder held with what this Point hands it.
    auto const apply = [&]( Point const& point, State state )
    {
      std::vector<bool> replaced( holderCount, false );
      if ( point.releases.has_value() )
      {
        replaced[*point.releases] = true;
      }
      for ( auto const& [holder, target] : point.handsOver )
      {
        replaced[holder] = true;
      }
      for ( std::size_t holder = 0; holder < holderCount; ++holder )
      {
        for ( std::size_t slot = 0; replaced[holder] && slot < width; ++slot )
        {
          state[( holder * width ) + slot] = false;
        }
      }
      handOver( state, point );
      return state;
    };

    // What a call may leave behind when it returns: whatever its callee, and
    // what that calls or jumps to, hands over anywhere.
    std::vector<std::optional<State>> summaries( nodes().size() );
    auto const summaryOf = [&]( std::uint32_t node ) -> State const&
    {
      if ( !summaries[node].has_value() )
      {
        std::vector<bool> reached( nodes().size(), false );
        mGraph->reachFrom( { node }, reached, false );
        State summary = none;
        for ( std::uint32_t index = 0; index < nodes().size(); ++index )
        {
          for ( Point const& point : reached[index] ? nodes()[index].points : std::vector<Point>{} )
          {
            handOver( summary, point );
          }
        }
        summaries[node] = std::move( summary );
      }
      return *summaries[node];
    };

    // What the hardware's own code and the program's data hand over, which
    // no path orders: all of it, wherever it is in memory.
    auto const unordered = [&]( auto const& present )
    {
      State state = none;
      for ( std::uint32_t index = 0; index < nodes().size(); ++index )
      {
        Node const& node = nodes()[index];
        if ( ( !node.code || always[index] ) && present( node ) )
        {
          for ( Point const& point : node.points )
          {
            handOver( state, point );
          }
        }
      }
      return state;
    };

    struct Flow
    {
      State held;
      std::vector<State> exits;
    };

    auto const flow = [&]( Position const& start, State const& in, State const& extra )
    {
      Flow result{ .held = in, .exits = std::vector<State>( phaseCount, none ) };
      std::map<std::uint64_t, State> at;
      std::vector<std::pair<std::uint32_t, std::uint32_t>> pending;
      auto const reach = [&]( std::uint32_t node, std::uint32_t point, State const& state )
      {
        if ( point >= nodes()[node].points.size() )
        {
          return;
        }
        auto [found, added] = at.try_emplace( ( static_cast<std::uint64_t>( node ) << 32U ) | point, none );
        if ( unite( found->second, state ) || added )
        {
          pending.emplace_back( node, point );
        }
      };
      std::uint32_t const first = mGraph->indexOf( start.where );
      reach( first, Graph::pointIndexOf( nodes()[first], start ), in );
      while ( !pending.empty() )
      {
        auto const [node, index] = pending.back();
        pending.pop_back();
        Point const& point = nodes()[node].points[index];
        State const before = at.at( ( static_cast<std::uint64_t>( node ) << 32U ) | index );
        State after = apply( point, before );
        unite( after, extra );
        unite( result.held, after );
        // A `.transition` leaves the Phase: what it is reached with is what
        // the Phase it enters begins with.
        if ( point.enters.has_value() )
        {
          unite( result.exits[point.enters->value], after );
          continue;
        }
        State onward = after;
        if ( point.calls )
        {
          for ( std::uint32_t const callee : point.callTargets )
          {
            unite( onward, summaryOf( callee ) );
          }
        }
        for ( std::uint32_t const next : point.successors )
        {
          reach( node, next, onward );
        }
        for ( Landing const& landing : point.transfers )
        {
          reach( landing.node, landing.point, after );
        }
      }
      return result;
    };

    auto const moduleOf = [&]( Node const& node ) -> Module const& { return symbols.moduleAt( node.where.module ); };

    // The Load: each `init` in order, from what the last left, with what the
    // load's own data and the hardware's code hand over.
    auto const loading = [&]( Node const& node )
    { return std::ranges::find( project.loadNeeds, node.where.module ) != project.loadNeeds.end(); };
    State const loadExtra = unordered( loading );
    State loadHeld = loadExtra;
    for ( LoadInit const& init : project.loadInits )
    {
      for ( ModuleIndex const module : project.loadNeeds )
      {
        Symbol const* const found = symbols.moduleAt( module ).symbols().find( init.name );
        if ( found == nullptr || found->kind != SymbolKind::LABEL )
        {
          continue;
        }
        std::vector<Symbol> const& table = symbols.moduleAt( module ).symbols().symbols();
        SymbolRef const symbol{ .module = module, .index = static_cast<std::uint32_t>( found - table.data() ) };
        if ( std::optional<Position> const start = positionOf( symbols, symbol ); start.has_value() )
        {
          unite( loadHeld, flow( *start, loadHeld, loadExtra ).held );
        }
        break;
      }
    }

    // The Phases, until no edge carries anything new.
    std::vector<State> in( phaseCount, none );
    std::vector<State> held( phaseCount, none );
    std::vector<State> extra( phaseCount, none );
    std::vector<std::optional<Position>> starts( phaseCount );
    for ( std::uint32_t phase = 0; phase < phaseCount; ++phase )
    {
      extra[phase] =
          unordered( [&]( Node const& node ) { return moduleOf( node ).residency().includes( PhaseIndex{ phase } ); } );
      if ( std::optional<SymbolRef> const entry = entrySymbolOf( phases, PhaseIndex{ phase }, symbols );
           entry.has_value() )
      {
        starts[phase] = positionOf( symbols, *entry );
      }
    }
    if ( phases.entry.value < phaseCount )
    {
      unite( in[phases.entry.value], loadHeld );
    }
    for ( bool changed = true; changed; )
    {
      changed = false;
      for ( std::uint32_t phase = 0; phase < phaseCount; ++phase )
      {
        State start = in[phase];
        unite( start, extra[phase] );
        held[phase] = start;
        if ( !starts[phase].has_value() )
        {
          continue;
        }
        Flow const result = flow( *starts[phase], start, extra[phase] );
        unite( held[phase], result.held );
        for ( std::uint32_t next = 0; next < phaseCount; ++next )
        {
          changed = unite( in[next], result.exits[next] ) || changed;
        }
      }
    }

    // A held Section holds what it names, and so on.
    auto const closure = [&]( State const& state )
    {
      std::vector<bool> reached( nodes().size(), false );
      std::vector<std::uint32_t> pending;
      for ( std::size_t bit = 0; bit < state.size(); ++bit )
      {
        std::uint32_t const node = handed[bit % width];
        if ( state[bit] && !reached[node] )
        {
          reached[node] = true;
          pending.push_back( node );
        }
      }
      while ( !pending.empty() )
      {
        std::uint32_t const node = pending.back();
        pending.pop_back();
        for ( std::uint32_t const named : nodes()[node].names )
        {
          if ( !reached[named] )
          {
            reached[named] = true;
            pending.push_back( named );
          }
        }
      }
      return reached;
    };
    std::vector<bool> const atLoad = closure( loadHeld );
    for ( std::uint32_t node = 0; node < nodes().size(); ++node )
    {
      if ( atLoad[node] )
      {
        holds.holdAtLoad( nodes()[node].where );
      }
    }
    for ( std::uint32_t phase = 0; phase < phaseCount; ++phase )
    {
      std::vector<bool> const inPhase = closure( held[phase] );
      for ( std::uint32_t node = 0; node < nodes().size(); ++node )
      {
        if ( inPhase[node] )
        {
          holds.hold( nodes()[node].where, PhaseIndex{ phase }, phaseCount );
        }
      }
    }
    return holds;
  }

private:
  [[nodiscard]] std::vector<Node> const& nodes() const
  {
    return mGraph->nodes();
  }

  Pruned const* mBuild;
  Graph const* mGraph;
};

} // namespace

Holds holdsOf( Graph const& graph, std::vector<bool> const& always )
{
  return HolderFlow{ graph }.computeHolds( always );
}

} // namespace nga::model::flow
