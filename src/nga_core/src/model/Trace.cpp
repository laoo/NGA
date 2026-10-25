#include "nga/model/Trace.hpp"

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

namespace nga::model
{

std::uint64_t Holds::keyOf( SectionRef where )
{
  return ( static_cast<std::uint64_t>( where.module.value ) << 32U ) | where.section.value;
}

void Holds::hold( SectionRef where, PhaseIndex phase, std::size_t phaseCount )
{
  auto [found, added] = mPhases.try_emplace( keyOf( where ), Residency{ phaseCount } );
  found->second.add( phase );
}

void Holds::holdAtLoad( SectionRef where )
{
  mAtLoad.insert( keyOf( where ) );
}

Residency Holds::phasesOf( SectionRef where, std::size_t phaseCount ) const
{
  auto const found = mPhases.find( keyOf( where ) );
  return found == mPhases.end() ? Residency{ phaseCount } : found->second;
}

bool Holds::atLoad( SectionRef where ) const
{
  return mAtLoad.contains( keyOf( where ) );
}

void Interference::allow( SectionRef a, SectionRef b )
{
  mAllowed.insert( keyOf( a, b ) );
}

bool Interference::mayShare( SectionRef a, SectionRef b ) const
{
  return mAllowed.contains( keyOf( a, b ) );
}

std::pair<std::uint64_t, std::uint64_t> Interference::keyOf( SectionRef a, SectionRef b )
{
  auto const flat = []( SectionRef where )
  { return ( static_cast<std::uint64_t>( where.module.value ) << 32U ) | where.section.value; };
  std::uint64_t const one = flat( a );
  std::uint64_t const two = flat( b );
  return one < two ? std::pair{ one, two } : std::pair{ two, one };
}

std::pair<Interference, Holds> trace( Pruned const& build, diag::DiagnosticSink& sink )
{
  flow::Graph graph = flow::readGraph( build, sink );
  flow::reportRunningOff( graph, sink );
  flow::reportFinalReturns( graph, sink );

  // The Container's way into the program, `ngaRun` and `ngaStart`, is
  // `root` only because nothing references it: it is entered once, with
  // nothing of the program active, and is the bottom of every stack as the
  // entry is. Taken as the hardware's, it would make everything the program
  // reaches active in every state, and no Temporary would share.
  std::vector<bool> starts( graph.nodes().size(), false );
  for ( std::string_view const name : { RUN_NAME, COLD_START_NAME } )
  {
    if ( std::optional<SymbolRef> const symbol = build.symbols().find( name ); symbol.has_value() )
    {
      if ( std::optional<flow::Position> const position = flow::positionOf( build.symbols(), *symbol );
           position.has_value() )
      {
        starts[graph.indexOf( position->where )] = true;
      }
    }
  }

  // A `root` Section is active in every state — the hardware enters it
  // whenever it likes — and so is everything it reaches.
  std::vector<bool> always( graph.nodes().size(), false );
  for ( std::uint32_t index = 0; index < graph.nodes().size(); ++index )
  {
    flow::Node const& node = graph.nodes()[index];
    if ( !starts[index] && build.reachable().includes( node.where ) &&
         build.symbols().moduleAt( node.where.module ).sectionAt( node.where.section ).isRoot() )
    {
      always[index] = true;
      graph.reachFrom( graph.nodes()[index].calls, always );
      graph.reachFrom( graph.nodes()[index].jumps, always );
    }
  }

  Interference interference = flow::interferenceOf( graph, always, sink );
  return { std::move( interference ), flow::holdsOf( graph, always ) };
}

} // namespace nga::model
