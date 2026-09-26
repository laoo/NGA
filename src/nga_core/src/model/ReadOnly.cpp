#include "nga/model/ReadOnly.hpp"

#include "nga/model/Evaluate.hpp"
#include "nga/model/PlacementClass.hpp"
#include "nga/model/Prune.hpp"
#include "nga/model/References.hpp"

#include <algorithm>
#include <optional>
#include <span>
#include <variant>

namespace nga::model
{

ReadOnly::ReadOnly( std::span<Module const> modules )
{
  mByModule.resize( modules.size() );
  mInRom.resize( modules.size() );
  for ( std::size_t module = 0; module < modules.size(); ++module )
  {
    mByModule[module].assign( modules[module].sections().size(), false );
    mInRom[module].assign( modules[module].sections().size(), false );
  }
}

void ReadOnly::mark( SectionRef where, bool inRom )
{
  mByModule[where.module.value][where.section.value] = true;
  mInRom[where.module.value][where.section.value] = inRom;
}

bool ReadOnly::includes( SectionRef where ) const
{
  return mByModule[where.module.value][where.section.value];
}

bool ReadOnly::standsInRom( SectionRef where ) const
{
  return mInRom[where.module.value][where.section.value];
}

namespace
{

/// Whether an address lies in a pool.
bool inPool( std::span<AddressRange const> pool, std::uint32_t address )
{
  return std::ranges::any_of(
      pool, [address]( AddressRange const& part ) { return address >= part.begin && address < part.end; } );
}

/// What one Chunk names, in the Sections the memory belongs to. A Slot is
/// declared indirection, so a store through one writes every Implementation.
class Named final : public ReferenceVisitor
{
public:
  explicit Named( GlobalSymbols const& symbols, ModuleIndex from ) : mSymbols( &symbols ), mFrom( from ) {}

  void reference( SymbolRef target ) override
  {
    if ( mSymbols->at( target ).kind == SymbolKind::SLOT )
    {
      for ( ResolvedImplementation const& implementation : mSymbols->implementationsOf( target ) )
      {
        add( implementation.target );
      }
      return;
    }
    add( target );
  }

  void localReference( LabelPosition target ) override
  {
    reached.push_back( SectionRef{ .module = mFrom, .section = target.section } );
  }

  std::vector<SectionRef> reached;

private:
  void add( SymbolRef target )
  {
    Symbol const& symbol = mSymbols->at( target );
    if ( auto const* const label = std::get_if<LabelPosition>( &symbol.value ); label != nullptr )
    {
      reached.push_back( SectionRef{ .module = target.module, .section = label->section } );
    }
  }

  GlobalSymbols const* mSymbols;
  ModuleIndex mFrom;
};

/// What the walk found about one Section: whether some Chunk writes it, and
/// where the first such Chunk stands, so that the finding points at the write
/// and not at the declaration alone.
struct Touched
{
  /// Where the first Chunk that writes it stands; nothing writes it where this
  /// is empty, so the two questions are one field.
  std::optional<diag::SourceSpan> writtenAt;
  bool taken = false;
};

} // namespace

ReadOnly readOnlySections( Pruned const& build, diag::DiagnosticSink& sink )
{
  GlobalSymbols const& symbols = build.symbols();
  diag::SourceManager const& sources = build.sources();
  std::span<Module const> const modules = build.modules();

  std::vector<std::vector<Touched>> touched( modules.size() );
  for ( std::size_t module = 0; module < modules.size(); ++module )
  {
    touched[module].assign( modules[module].sections().size(), Touched{} );
  }

  auto const note = [&touched]( SectionRef where, ReferenceKind kind, diag::SourceSpan span )
  {
    Touched& one = touched[where.module.value][where.section.value];
    if ( ( kind == ReferenceKind::WRITE || kind == ReferenceKind::READ_WRITE ) && !one.writtenAt.has_value() )
    {
      one.writtenAt = span;
    }
    if ( kind == ReferenceKind::ESCAPE )
    {
      one.taken = true;
    }
  };

  // Only what Prune kept: a store in code nothing reaches reaches no memory
  // either, and holding a Section out of ROM for it would be an address
  // decided by dead code.
  for ( std::uint32_t index = 0; index < modules.size(); ++index )
  {
    ModuleIndex const module{ index };
    Module const& one = symbols.moduleAt( module );
    for ( std::uint32_t at = 0; at < one.sections().size(); ++at )
    {
      SectionRef const where{ .module = module, .section = SectionIndex{ at } };
      if ( !build.reachable().includes( where ) )
      {
        continue;
      }
      Section const& section = one.sectionAt( where.section );
      auto const read = [&]( Chunk const& chunk )
      {
        ReferenceKind const kind = referenceKindOf( sources, chunk );
        Named named{ symbols, module };
        for ( syntax::ExpressionPtr const& item : section.itemsOf( chunk ) )
        {
          walkReferences( symbols, sources, module, *item, named );
        }
        for ( SectionRef const target : named.reached )
        {
          note( target, kind, chunk.span );
        }
      };
      for ( std::uint32_t position = 0; position < section.chunks().size(); ++position )
      {
        Chunk const& chunk = section.chunks()[position];

        // A macro use encodes its expansion and not its arguments: what each
        // statement of the body does is that statement's, and the use itself
        // does nothing.
        if ( std::holds_alternative<MacroUseContent>( chunk.content ) )
        {
          for ( Chunk const& inner : section.innerChunksOf( ChunkIndex{ position } ) )
          {
            read( inner );
          }
          continue;
        }
        read( chunk );
      }
    }
  }

  ReadOnly result{ modules };
  for ( std::uint32_t index = 0; index < modules.size(); ++index )
  {
    ModuleIndex const module{ index };
    Module const& one = symbols.moduleAt( module );
    for ( std::uint32_t at = 0; at < one.sections().size(); ++at )
    {
      SectionRef const where{ .module = module, .section = SectionIndex{ at } };
      Section const& section = one.sectionAt( where.section );
      Touched const& found = touched[index][at];

      // A store into a Proc is self-modifying code and is seen like any other
      // store; the word cannot say otherwise about what the tool can see.
      if ( found.writtenAt.has_value() && section.saysReadOnly() )
      {
        diag::SourceSpan const& write = *found.writtenAt;
        sink.add( diag::diagnostic( diag::DiagnosticId::READONLY_WRITTEN )
                      .at( write.begin, write.length )
                      .arg( "section", one.displayNameOf( where.section, sources ) )
                      .note( diag::diagnostic( diag::DiagnosticId::READONLY_DECLARED_HERE )
                                 .at( section.span().begin, section.span().length ) ) );
        continue;
      }

      // Nothing to be read-only: the bytes ROM would hold are the Section's
      // own, and a Section of reservations alone has none.
      bool const hasBytes = section.emitsBytes();
      if ( section.saysReadOnly() && !hasBytes )
      {
        sink.add( diag::diagnostic( diag::DiagnosticId::READONLY_WITHOUT_BYTES )
                      .at( section.span().begin, section.span().length )
                      .arg( "section", one.displayNameOf( where.section, sources ) ) );
        continue;
      }
      if ( found.writtenAt.has_value() || !hasBytes || section.isTemporary() || section.isMovable() ||
           section.placement() == PlacementClass::ZEROPAGE )
      {
        continue;
      }
      // An address of code taken is a jump and never a write — `.own` says so
      // — so a Proc needs no word for it. An address of data taken is what
      // nothing in the model follows, and the word is what says it is read
      // through.
      if ( found.taken && !section.isProc() && !section.saysReadOnly() )
      {
        continue;
      }

      // And whether that puts it in ROM. A pin decides for itself, because a
      // pin is the author's word and the solver satisfies it rather than
      // refusing it: a pin into ROM stands in ROM, and one into RAM stands in
      // RAM. Place reports a pin that lands in ROM and is written.
      bool inRom = !build.target().pools.readOnly.empty();
      if ( section.pinnedAddress() != nullptr )
      {
        std::optional<std::int64_t> const address =
            declaredValueOf( sources, symbols, &build.charsets(), where.module, *section.pinnedAddress() );
        inRom = address.has_value() && *address >= 0 &&
                inPool( build.target().pools.readOnly, static_cast<std::uint32_t>( *address ) );
      }
      result.mark( where, inRom );
    }
  }
  return result;
}

} // namespace nga::model
