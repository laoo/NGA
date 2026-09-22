#include "nga/model/Merge.hpp"

#include <string>
#include <variant>

namespace nga::model
{

std::optional<SymbolRef> GlobalSymbols::find( std::string_view name ) const
{
  auto const entry = mByName.find( name );
  if ( entry == mByName.end() )
  {
    return std::nullopt;
  }
  return entry->second;
}

std::optional<SymbolRef> GlobalSymbols::add( std::string_view name, SymbolRef where )
{
  auto const [entry, inserted] = mByName.try_emplace( name, where );
  if ( inserted )
  {
    return std::nullopt;
  }
  return entry->second;
}

std::optional<SymbolRef> GlobalSymbols::lookup( ModuleIndex home, std::string_view name ) const
{
  SymbolTable const& own = moduleAt( home ).symbols();
  if ( Symbol const* const local = own.find( name ); local != nullptr )
  {
    auto const index = static_cast<std::uint32_t>( local - own.symbols().data() );
    return SymbolRef{ .module = home, .index = index };
  }
  return find( name );
}

std::optional<SymbolRef>
GlobalSymbols::lookupScoped( ModuleIndex home, std::string_view scope, std::string_view name ) const
{
  std::string_view rest = scope;
  while ( true )
  {
    std::string candidate{ rest };
    candidate += name;
    if ( std::optional<SymbolRef> const found = lookup( home, candidate ); found.has_value() )
    {
      return found;
    }
    if ( rest.empty() )
    {
      return std::nullopt;
    }
    rest.remove_suffix( 1 );
    std::size_t const dot = rest.rfind( '.' );
    rest = dot == std::string_view::npos ? std::string_view{} : rest.substr( 0, dot + 1 );
  }
}

std::optional<SymbolRef>
GlobalSymbols::resolveText( ModuleIndex from, syntax::Expression const& node, std::string_view text ) const
{
  Module const& owner = moduleAt( from );
  return lookupScoped( owner.homeOf( &node ).value_or( from ), owner.scopeOf( &node ).value_or( "" ), text );
}

std::optional<SectionRef> sectionNamedBy( GlobalSymbols const& symbols, SymbolRef where )
{
  // Every Label of a Section reaches its attributes, wherever in it the
  // Label stands: the attributes are the Section's, and the Label is how
  // source names one.
  auto const* const label = std::get_if<LabelPosition>( &symbols.at( where ).value );
  if ( label == nullptr )
  {
    return std::nullopt;
  }
  return SectionRef{ .module = where.module, .section = label->section };
}

namespace
{

/// Whether this Symbol is the Label a `.proc` defines, which names the Proc's
/// Section and opens the scope its own names lie under.
bool opensItsOwnScope( Module const& module, Symbol const& symbol )
{
  // A Slot that takes or returns names its Temporaries inside itself, `step.a`
  // beside `.slot step, vector`, and needs no exception of the kind
  // docs/decisions/0046-a-proc-is-a-scope.md wrote for a Proc: a Slot is no
  // Label, and what is no Label has no attributes at all (`NGA2224`), so
  // nothing it is on the left of can be one — see
  // docs/decisions/0174-a-slot-takes-and-returns.md.
  if ( symbol.kind == SymbolKind::SLOT )
  {
    return true;
  }
  auto const* const label = std::get_if<LabelPosition>( &symbol.value );
  return label != nullptr && label->chunk == ChunkIndex{ 0 } && module.sectionAt( label->section ).isProc();
}

} // namespace

GlobalSymbols merge( std::span<Module const> modules, diag::DiagnosticSink& sink )
{
  GlobalSymbols globals{ modules };

  // The Target's named Regions and the exports of the Modules the tool
  // added first, so that a Module's Symbol of the same name is what
  // collides, whether it is exported or not: a register's name is a fact
  // about the hardware, and a Module quietly shadowing it is how a local
  // equate drifts from the variant; a name the tool defines is one the
  // routine and the Container reach by name, and a Module shadowing it
  // would take its place in that Module alone. Two Regions of one name were
  // refused when the Project was read. A tool Module is named with a dot,
  // which no Module an author writes can be — see docs/spec/transition.md.
  for ( std::uint32_t index = 0; index < modules.size(); ++index )
  {
    bool const tools = modules[index].name().starts_with( "nga." );
    std::vector<Symbol> const& symbols = modules[index].symbols().symbols();
    for ( std::uint32_t position = 0; position < symbols.size(); ++position )
    {
      if ( symbols[position].kind == SymbolKind::REGION || ( tools && symbols[position].exported ) )
      {
        globals.add( symbols[position].name, SymbolRef{ .module = ModuleIndex{ index }, .index = position } );
      }
      // Every Namespace a qualified name lies under, exported or not, so
      // that `one` alone can be called what it is.
      std::string_view const name = symbols[position].name;
      for ( std::size_t dot = name.find( '.' ); dot != std::string_view::npos; dot = name.find( '.', dot + 1 ) )
      {
        globals.noteNamespace( std::string{ name.substr( 0, dot ) } );
      }
    }
  }

  for ( std::uint32_t index = 0; index < modules.size(); ++index )
  {
    Module const& module = modules[index];
    bool const tools = module.name().starts_with( "nga." );
    std::vector<Symbol> const& symbols = module.symbols().symbols();

    for ( std::uint32_t position = 0; position < symbols.size(); ++position )
    {
      Symbol const& symbol = symbols[position];
      if ( symbol.kind == SymbolKind::REGION || ( tools && symbol.exported ) )
      {
        continue;
      }

      // A Symbol named as a Namespace is: `one` the Section and `one.x`
      // cannot both stand, since `one.x` would then be an attribute of one
      // and a member of the other. A Proc is the exception it makes itself:
      // its Label opens the scope its own names lie in, and the three
      // attributes are refused as names inside it, so `worker.runtimeSectionSize`
      // and `worker.inner` stay apart — see
      // docs/decisions/0046-a-proc-is-a-scope.md.
      if ( globals.isNamespace( symbol.name ) && !opensItsOwnScope( module, symbol ) )
      {
        sink.add( diag::diagnostic( diag::DiagnosticId::NAMESPACE_NAME_COLLISION )
                      .at( symbol.definition.begin, symbol.definition.length )
                      .arg( "symbol", std::string{ symbol.name } ) );
        continue;
      }

      if ( std::optional<SymbolRef> const taken = globals.find( symbol.name ); taken.has_value() )
      {
        Symbol const& named = globals.at( *taken );
        if ( modules[taken->module.value].name() == "nga.regions" )
        {
          sink.add( diag::diagnostic( diag::DiagnosticId::REGION_NAME_COLLISION )
                        .at( symbol.definition.begin, symbol.definition.length )
                        .arg( "symbol", std::string{ symbol.name } )
                        .note( diag::diagnostic( diag::DiagnosticId::REGION_DECLARED_HERE )
                                   .at( named.definition.begin, named.definition.length )
                                   .arg( "region", std::string{ named.name } ) ) );
          continue;
        }
        if ( modules[taken->module.value].name().starts_with( "nga." ) )
        {
          sink.add( diag::diagnostic( diag::DiagnosticId::TOOL_NAME_COLLISION )
                        .at( symbol.definition.begin, symbol.definition.length )
                        .arg( "symbol", std::string{ symbol.name } )
                        .note( diag::diagnostic( diag::DiagnosticId::TOOL_NAME_DEFINED_HERE )
                                   .at( named.definition.begin, named.definition.length )
                                   .arg( "symbol", std::string{ named.name } ) ) );
          continue;
        }
      }

      if ( !symbol.exported )
      {
        continue;
      }

      std::optional<SymbolRef> const existing =
          globals.add( symbol.name, SymbolRef{ .module = ModuleIndex{ index }, .index = position } );
      if ( !existing.has_value() )
      {
        continue;
      }

      Symbol const& previous = globals.at( *existing );
      sink.add( diag::diagnostic( diag::DiagnosticId::DUPLICATE_EXPORT )
                    .at( symbol.definition.begin, symbol.definition.length )
                    .arg( "symbol", std::string{ symbol.name } )
                    .note( diag::diagnostic( diag::DiagnosticId::PREVIOUS_DEFINITION )
                               .at( previous.definition.begin, previous.definition.length )
                               .arg( "symbol", std::string{ previous.name } ) ) );
    }
  }

  // `as TYPE` on a Proc: the type is a Proc of the program, and the member
  // declares no bytes of its own, since its arguments are the type's — see
  // docs/decisions/0065-handlers.md. Whether it reads them as it should is
  // the author's word, as `temporary` is.
  for ( std::uint32_t index = 0; index < modules.size(); ++index )
  {
    ModuleIndex const home{ index };
    Module const& module = modules[index];
    for ( Member const& member : module.members() )
    {
      std::optional<SymbolRef> const type = globals.lookupScoped( home, member.scope, member.type );
      if ( !type.has_value() )
      {
        sink.add( diag::diagnostic( diag::DiagnosticId::UNKNOWN_SYMBOL )
                      .at( member.span.begin, member.span.length )
                      .arg( "symbol", std::string{ member.type } ) );
        continue;
      }
      auto const* const label = std::get_if<LabelPosition>( &globals.at( *type ).value );
      bool const isProc = label != nullptr && !label->inner.has_value() &&
                          modules[type->module.value].sections()[label->section.value].isProc();
      if ( !isProc )
      {
        sink.add( diag::diagnostic( diag::DiagnosticId::AS_ATTRIBUTE )
                      .at( member.span.begin, member.span.length )
                      .arg( "name", std::string{ member.type } )
                      .arg( "reason",
                            "names what is no proc, and a function type is the proc whose temporaries "
                            "hold the arguments" ) );
        continue;
      }
      Section const& section = module.sections()[member.section.value];
      if ( !section.signature().isEmpty() )
      {
        // The Proc by the Label that stands for it, as every finding names a
        // Section — and by its own symbols, since Merge reads no source.
        std::string_view name;
        for ( Symbol const& own : module.symbols().symbols() )
        {
          auto const* const at = std::get_if<LabelPosition>( &own.value );
          if ( at != nullptr && at->section == member.section && !at->inner.has_value() )
          {
            name = own.name;
            break;
          }
        }
        sink.add( diag::diagnostic( diag::DiagnosticId::AS_WITH_SIGNATURE )
                      .at( member.span.begin, member.span.length )
                      .arg( "proc", std::string{ name } )
                      .arg( "type", std::string{ member.type } )
                      .arg( "what", section.signature().arguments.empty() ? "a result" : "arguments" ) );
      }
    }
  }

  // `.implements`, resolved: the Slot as this Module sees it, the Symbol as
  // this Module defines it. Which Implementations may coexist is a rule about
  // Phases, and belongs to the type check, which has them.
  for ( std::uint32_t index = 0; index < modules.size(); ++index )
  {
    ModuleIndex const home{ index };
    for ( Implementation const& implementation : modules[index].implementations() )
    {
      std::optional<SymbolRef> const slot = globals.lookupScoped( home, implementation.scope, implementation.slot );
      if ( !slot.has_value() )
      {
        sink.add( diag::diagnostic( diag::DiagnosticId::UNKNOWN_SYMBOL )
                      .at( implementation.slotSpan.begin, implementation.slotSpan.length )
                      .arg( "symbol", std::string{ implementation.slot } ) );
        continue;
      }
      if ( globals.at( *slot ).kind != SymbolKind::SLOT )
      {
        sink.add( diag::diagnostic( diag::DiagnosticId::NOT_A_SLOT )
                      .at( implementation.slotSpan.begin, implementation.slotSpan.length )
                      .arg( "symbol", std::string{ implementation.slot } ) );
        continue;
      }

      // The Symbol of this Module, in the Namespace the directive stood in.
      std::vector<Symbol> const& own = modules[index].symbols().symbols();
      std::optional<SymbolRef> target;
      for ( std::string_view rest = implementation.scope;; )
      {
        std::string candidate{ rest };
        candidate += implementation.symbol;
        for ( std::uint32_t position = 0; position < own.size(); ++position )
        {
          if ( own[position].name == candidate && own[position].kind == SymbolKind::LABEL )
          {
            target = SymbolRef{ .module = home, .index = position };
            break;
          }
        }
        if ( target.has_value() || rest.empty() )
        {
          break;
        }
        rest.remove_suffix( 1 );
        std::size_t const dot = rest.rfind( '.' );
        rest = dot == std::string_view::npos ? std::string_view{} : rest.substr( 0, dot + 1 );
      }
      if ( !target.has_value() )
      {
        sink.add( diag::diagnostic( diag::DiagnosticId::IMPLEMENTATION_NOT_HERE )
                      .at( implementation.span.begin, implementation.span.length )
                      .arg( "symbol", std::string{ implementation.symbol } ) );
        continue;
      }
      globals.addImplementation( *slot, ResolvedImplementation{ .target = *target, .span = implementation.span } );
    }
  }

  return globals;
}

void GlobalSymbols::addImplementation( SymbolRef slot, ResolvedImplementation implementation )
{
  if ( mImplementations.size() < mModules.size() )
  {
    mImplementations.resize( mModules.size() );
  }
  std::vector<std::vector<ResolvedImplementation>>& ofModule = mImplementations[slot.module.value];
  if ( ofModule.size() <= slot.index )
  {
    ofModule.resize( slot.index + 1 );
  }
  ofModule[slot.index].push_back( implementation );
}

std::span<ResolvedImplementation const> GlobalSymbols::implementationsOf( SymbolRef slot ) const
{
  if ( slot.module.value >= mImplementations.size() || slot.index >= mImplementations[slot.module.value].size() )
  {
    return {};
  }
  return mImplementations[slot.module.value][slot.index];
}

} // namespace nga::model
