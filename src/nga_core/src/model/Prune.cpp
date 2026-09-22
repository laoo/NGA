#include "nga/model/Prune.hpp"

#include "nga/model/Evaluate.hpp"
#include "nga/model/References.hpp"
#include "nga/model/Transition.hpp"

#include <spdlog/spdlog.h>

#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace nga::model
{

Reachable::Reachable( std::span<Module const> modules )
{
  mByModule.resize( modules.size() );
  for ( std::size_t module = 0; module < modules.size(); ++module )
  {
    mByModule[module].assign( modules[module].sections().size(), false );
  }
}

Reachable Reachable::all( std::span<Module const> modules )
{
  Reachable result{ modules };
  for ( std::vector<bool>& sections : result.mByModule )
  {
    sections.assign( sections.size(), true );
  }
  return result;
}

bool Reachable::keep( SectionRef where )
{
  std::vector<bool>::reference slot = mByModule[where.module.value][where.section.value];
  if ( slot )
  {
    return false;
  }
  slot = true;
  return true;
}

bool Reachable::includes( SectionRef where ) const
{
  return mByModule[where.module.value][where.section.value];
}

namespace
{

/// The Section a Symbol's memory is in: a Label's, or the Section itself.
/// Nothing for a Constant, a Charset or a Slot, which are not memory.
std::optional<SectionRef> memoryOf( GlobalSymbols const& symbols, SymbolRef where )
{
  Symbol const& symbol = symbols.at( where );
  if ( auto const* const label = std::get_if<LabelPosition>( &symbol.value ); label != nullptr )
  {
    return SectionRef{ .module = where.module, .section = label->section };
  }
  return std::nullopt;
}

/// The walk: a worklist of Sections reached, each visited once for what its
/// Chunks name.
class Pruner final : public ReferenceVisitor
{
public:
  explicit Pruner( Merged const& build ) : mBuild( &build ), mReachable( build.modules() ) {}

  void reach( SectionRef where )
  {
    if ( mReachable.keep( where ) )
    {
      mPending.push_back( where );
    }
  }

  void reachSymbol( std::optional<SymbolRef> where )
  {
    if ( !where.has_value() )
    {
      return;
    }
    if ( std::optional<SectionRef> const memory = memoryOf( mBuild->symbols(), *where ); memory.has_value() )
    {
      reach( *memory );
    }
  }

  /// A use of a Slot reaches its Cell, and the Cell's Chunk reaches every
  /// Implementation — the edges write each of them into the Cell in turn.
  void reference( SymbolRef target ) override
  {
    Symbol const& symbol = mBuild->symbols().at( target );
    if ( auto const* const slot = std::get_if<SlotDeclaration>( &symbol.value ); slot != nullptr )
    {
      if ( slot->cell.has_value() )
      {
        reach( *slot->cell );
      }
      return;
    }
    reachSymbol( target );
  }

  void localReference( LabelPosition target ) override
  {
    reach( SectionRef{ .module = mFrom, .section = target.section } );
  }

  void reachEntryOf( PhaseIndex phase )
  {
    reachSymbol( entrySymbolOf( mBuild->phases(), phase, mBuild->symbols() ) );
  }

  void visit( SectionRef where )
  {
    mFrom = where.module;
    Module const& module = mBuild->symbols().moduleAt( where.module );
    Section const& section = module.sectionAt( where.section );

    // A Proc falls through into what its `then` names, which no Chunk
    // encodes and which is reached all the same.
    if ( std::optional<SectionIndex> const next = section.next(); next.has_value() )
    {
      reach( SectionRef{ .module = where.module, .section = *next } );
    }
    for ( std::uint32_t index = 0; index < section.chunks().size(); ++index )
    {
      Chunk const& chunk = section.chunks()[index];

      // A macro use encodes its expansion and not its arguments, which stand
      // cloned inside it.
      if ( std::holds_alternative<MacroUseContent>( chunk.content ) )
      {
        for ( Chunk const& inner : section.innerChunksOf( ChunkIndex{ index } ) )
        {
          for ( syntax::ExpressionPtr const& item : section.itemsOf( inner ) )
          {
            walkReferences( mBuild->symbols(), mBuild->sources(), where.module, *item, *this );
          }
        }
        continue;
      }
      for ( syntax::ExpressionPtr const& item : section.itemsOf( chunk ) )
      {
        walkReferences( mBuild->symbols(), mBuild->sources(), where.module, *item, *this );
      }

      // A `.transition` reaches the routine and the entry of the Phase it
      // enters; what the edge loads is what reachability filters, and waits
      // in a Frame no Chunk names. The Cell is a byte and reaches nothing.
      if ( auto const* const transition = std::get_if<TransitionContent>( &chunk.content ); transition != nullptr )
      {
        reachSymbol( mBuild->symbols().find( TRANSITION_ROUTINE_NAME ) );
        if ( transition->target.has_value() )
        {
          reachEntryOf( *transition->target );
        }
      }
      else if ( auto const* const cell = std::get_if<SlotCellContent>( &chunk.content ); cell != nullptr )
      {
        SymbolRef const slot{ .module = cell->module, .index = cell->symbol };
        for ( ResolvedImplementation const& implementation : mBuild->symbols().implementationsOf( slot ) )
        {
          reachSymbol( implementation.target );
        }
      }
    }
  }

  Reachable run()
  {
    while ( !mPending.empty() )
    {
      SectionRef const next = mPending.back();
      mPending.pop_back();
      visit( next );
    }
    return std::move( mReachable );
  }

private:
  Merged const* mBuild;
  Reachable mReachable;
  std::vector<SectionRef> mPending;
  ModuleIndex mFrom;
};

} // namespace

Reachable prune( Merged const& build, diag::DiagnosticSink& sink )
{
  std::span<Module const> const modules = build.modules();
  Pruner pruner{ build };

  bool anyRoot = false;
  if ( std::optional<SymbolRef> const entry = entrySymbolOf( build.phases(), build.phases().entry, build.symbols() );
       entry.has_value() )
  {
    pruner.reachSymbol( entry );
    anyRoot = true;
  }
  for ( std::uint32_t module = 0; module < modules.size(); ++module )
  {
    for ( std::uint32_t index = 0; index < modules[module].sections().size(); ++index )
    {
      if ( modules[module].sections()[index].isRoot() )
      {
        pruner.reach( SectionRef{ .module = ModuleIndex{ module }, .section = SectionIndex{ index } } );
        anyRoot = true;
      }
    }
  }

  // Reachability needs somewhere to start from. A program that names no
  // start is memory rather than a program, and a raw image is exactly that;
  // dropping everything would not be a program either.
  if ( !anyRoot )
  {
    return Reachable::all( modules );
  }

  Reachable reachable = pruner.run();

  for ( std::uint32_t module = 0; module < modules.size(); ++module )
  {
    Module const& one = modules[module];
    for ( std::uint32_t index = 0; index < one.sections().size(); ++index )
    {
      Section const& section = one.sections()[index];
      SectionRef const where{ .module = ModuleIndex{ module }, .section = SectionIndex{ index } };
      if ( reachable.includes( where ) )
      {
        continue;
      }
      spdlog::debug( "prune: `{}` is reached by nothing and dropped",
                     one.displayNameOf( where.section, build.sources() ) );
      if ( section.pinnedAddress() == nullptr )
      {
        continue;
      }
      // A pin is a claim that something reaches this address, and the
      // something may be hardware that no Chunk names — which is the one
      // thing this Step cannot see and the author can say.
      std::optional<std::int64_t> const address = declaredValueOf(
          build.sources(), build.symbols(), &build.charsets(), where.module, *section.pinnedAddress() );
      sink.add( diag::diagnostic( diag::DiagnosticId::UNREACHABLE_PIN )
                    .at( section.span().begin, section.span().length )
                    .arg( "section", one.displayNameOf( where.section, build.sources() ) )
                    .arg( "address", address.value_or( 0 ) ) );
    }
  }

  return reachable;
}

namespace
{

/// Every Section a `.root` taking names, through the same walk Prune makes.
class RootTaker final : public ReferenceVisitor
{
public:
  RootTaker( GlobalSymbols const& symbols, ModuleIndex from ) : mSymbols( &symbols ), mFrom( from ) {}

  void reference( SymbolRef target ) override
  {
    if ( std::optional<SectionRef> const memory = memoryOf( *mSymbols, target ); memory.has_value() )
    {
      named.push_back( *memory );
    }
  }

  void localReference( LabelPosition target ) override
  {
    named.push_back( SectionRef{ .module = mFrom, .section = target.section } );
  }

  std::vector<SectionRef> named;

private:
  GlobalSymbols const* mSymbols;
  ModuleIndex mFrom;
};

} // namespace

void markRoots( diag::SourceManager const& sources,
                GlobalSymbols const& symbols,
                std::span<Module> modules,
                diag::DiagnosticSink& sink )
{
  for ( std::uint32_t module = 0; module < modules.size(); ++module )
  {
    for ( std::uint32_t index = 0; index < modules[module].sections().size(); ++index )
    {
      Section const& holder = modules[module].sections()[index];
      auto const take = [&]( Chunk const& chunk )
      {
        if ( chunk.taking != Taking::ROOT )
        {
          return;
        }
        RootTaker taker{ symbols, ModuleIndex{ module } };
        for ( syntax::ExpressionPtr const& item : holder.itemsOf( chunk ) )
        {
          walkReferences( symbols, sources, ModuleIndex{ module }, *item, taker );
        }
        if ( taker.named.empty() )
        {
          sink.add(
              diag::diagnostic( diag::DiagnosticId::ROOT_NAMES_NOTHING ).at( chunk.span.begin, chunk.span.length ) );
        }
        for ( SectionRef const target : taker.named )
        {
          modules[target.module.value].sectionAt( target.section ).setRoot();
        }
      };
      for ( std::uint32_t chunk = 0; chunk < holder.chunks().size(); ++chunk )
      {
        take( holder.chunks()[chunk] );
        for ( Chunk const& inner : holder.innerChunksOf( ChunkIndex{ chunk } ) )
        {
          take( inner );
        }
      }
    }
  }
}

} // namespace nga::model
