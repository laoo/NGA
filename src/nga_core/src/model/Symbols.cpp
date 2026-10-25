#include "nga/model/Symbols.hpp"

#include "nga/model/Merge.hpp"
#include "nga/model/Module.hpp"
#include "nga/model/Symbol.hpp"
#include "nga/model/Transition.hpp"
#include "nga/syntax/Expression.hpp"
#include "nga/syntax/Literal.hpp"

#include <algorithm>
#include <cstddef>
#include <map>
#include <optional>
#include <set>
#include <unordered_set>
#include <utility>
#include <variant>

namespace nga::model
{

namespace
{

using Skipped = std::unordered_set<syntax::Expression const*>;

/// Every place a name is written, each attributed to the Symbol the name
/// resolves to and no further, and each place once however many ways reach it.
class Uses
{
public:
  Uses( Merged const& build, std::vector<SymbolFacts>& facts ) : mBuild( &build ), mFacts( &facts )
  {
    for ( std::size_t i = 0; i < facts.size(); ++i )
    {
      mIndex.emplace( std::pair{ facts[i].where.module.value, facts[i].where.index }, i );
    }
  }

  void add( std::optional<SymbolRef> where, diag::SourceSpan span )
  {
    if ( !where.has_value() )
    {
      return;
    }
    auto const found = mIndex.find( { where->module.value, where->index } );
    std::optional<SymbolSite> const site = siteOf( mBuild->sources(), span );
    if ( found == mIndex.end() || !site.has_value() || !mSeen.insert( span.begin.rawOffset() ).second )
    {
      return;
    }
    ( *mFacts )[found->second].uses.push_back( *site );
  }

  /// Every name an expression writes. A name a local label stands for is no
  /// Symbol's, and neither is a macro's parameter, which `skip` holds; a
  /// literal's character set prefix is a use of the Charset it names.
  void walk( ModuleIndex from, syntax::Expression const& node, Skipped const& skip )
  {
    GlobalSymbols const& symbols = mBuild->symbols();
    diag::SourceManager const& sources = mBuild->sources();
    if ( skip.contains( &node ) )
    {
      return;
    }
    switch ( node.kind )
    {
    case syntax::ExpressionKind::NAME:
      if ( !symbols.moduleAt( from ).localTarget( &node ).has_value() )
      {
        add( symbols.resolveText( from, node, sources.textOf( node.token.span() ) ), node.token.span() );
      }
      return;
    case syntax::ExpressionKind::STRING:
    case syntax::ExpressionKind::CHARACTER:
    {
      std::string_view const prefix = syntax::quotedOf( sources.textOf( node.token.span() ) ).charset;
      if ( !prefix.empty() )
      {
        add( symbols.resolveText( from, node, prefix ),
             diag::SourceSpan{ .begin = node.token.location, .length = static_cast<std::uint32_t>( prefix.size() ) } );
      }
      return;
    }
    case syntax::ExpressionKind::ATTRIBUTE:
      // `level.levelMap` is one name before it is an attribute of anything,
      // so it is tried whole first -- the same order the model resolves it in.
      if ( std::optional<std::string> const dotted = syntax::dottedNameOf( sources, node ); dotted.has_value() )
      {
        if ( std::optional<SymbolRef> const where = symbols.resolveText( from, syntax::leftmostOf( node ), *dotted );
             where.has_value() )
        {
          add( where, node.span );
          return;
        }
      }
      break;
    default:
      break;
    }

    // A node holds at most two: `left` alone for a unary or an attribute's
    // receiver, both for a binary.
    if ( node.left != nullptr )
    {
      walk( from, *node.left, skip );
    }
    if ( node.right != nullptr )
    {
      walk( from, *node.right, skip );
    }
  }

  /// A name written as a token rather than an expression, read in the
  /// Namespace it stood in as the model reads it.
  void named( ModuleIndex from, std::string_view scope, syntax::Token const& name )
  {
    std::string_view const text = mBuild->sources().textOf( name.span() );
    add( mBuild->symbols().lookupScoped( from, scope, text ), name.span() );
  }

  /// Every expression of a Section: its attributes, its Chunks' items and the
  /// conditions of its branches, whichever branch the build would take.
  void section( ModuleIndex from, Section const& section, Skipped const& skip )
  {
    for ( syntax::Expression const* attribute : { section.pinnedAddress(), section.alignment(), section.boundary() } )
    {
      if ( attribute != nullptr )
      {
        walk( from, *attribute, skip );
      }
    }
    for ( Chunk const& chunk : section.chunks() )
    {
      for ( syntax::ExpressionPtr const& item : section.itemsOf( chunk ) )
      {
        if ( item != nullptr )
        {
          walk( from, *item, skip );
        }
      }
    }
    for ( Conditional const& conditional : section.conditionals() )
    {
      for ( Branch const& branch : conditional.branches )
      {
        if ( branch.condition != nullptr )
        {
          walk( from, *branch.condition, skip );
        }
      }
    }
  }

private:
  Merged const* mBuild;
  std::vector<SymbolFacts>* mFacts;
  std::map<std::pair<std::uint32_t, std::uint32_t>, std::size_t> mIndex;
  std::set<std::uint32_t> mSeen;
};

/// The Symbol named `name` among one Module's own, or nothing.
std::optional<SymbolRef> ownSymbol( GlobalSymbols const& symbols, ModuleIndex module, std::string_view name )
{
  std::vector<Symbol> const& table = symbols.moduleAt( module ).symbols().symbols();
  auto const found = std::ranges::find_if( table, [name]( Symbol const& one ) { return one.name == name; } );
  if ( found == table.end() )
  {
    return std::nullopt;
  }
  return SymbolRef{ .module = module, .index = static_cast<std::uint32_t>( found - table.begin() ) };
}

/// Every place a Module writes a name.
void collectModule( Uses& uses, Merged const& build, ModuleIndex from )
{
  GlobalSymbols const& symbols = build.symbols();
  Module const& module = symbols.moduleAt( from );
  Skipped const none;

  for ( Symbol const& symbol : module.symbols().symbols() )
  {
    if ( auto const* const value = std::get_if<syntax::ExpressionPtr>( &symbol.value );
         value != nullptr && *value != nullptr )
    {
      uses.walk( from, **value, none );
    }
  }

  for ( Section const& section : module.sections() )
  {
    uses.section( from, section, none );
    // `.endp then NAME`, and `in`, `also`, `under` and `as`: names the model
    // reads in the Namespace the Section stood in.
    if ( section.then().has_value() )
    {
      uses.named( from, section.thenScope(), *section.then() );
    }
    for ( std::optional<syntax::Token> const& name : { section.paneName(), section.underName(), section.asName() } )
    {
      if ( name.has_value() )
      {
        uses.named( from, section.scope(), *name );
      }
    }
    for ( syntax::Token const& also : section.alsoIn() )
    {
      uses.named( from, section.scope(), also );
    }
  }

  // A macro's body is written once and read at every use, so its names are
  // its own: a parameter, or a name a `.case` bound, stands for an argument.
  for ( MacroDefinition const& macro : module.macros() )
  {
    Skipped parameters;
    for ( auto const& [node, index] : macro.parameterUses )
    {
      parameters.insert( node );
    }
    for ( auto const& [node, binding] : macro.caseUses )
    {
      parameters.insert( node );
    }
    uses.section( from, macro.body, parameters );
  }

  for ( Assertion const& assertion : module.assertions() )
  {
    if ( assertion.condition != nullptr )
    {
      uses.walk( from, *assertion.condition, none );
    }
  }

  for ( ExportSite const& site : module.exportSites() )
  {
    uses.add( ownSymbol( symbols, from, site.symbol ), site.span );
  }
  for ( Implementation const& implementation : module.implementations() )
  {
    uses.add( symbols.lookupScoped( from, implementation.scope, implementation.slot ), implementation.slotSpan );
    uses.add( symbols.lookupScoped( from, implementation.scope, implementation.symbol ), implementation.symbolSpan );
  }
  for ( TransformDeclaration const& transform : module.transforms() )
  {
    uses.add( symbols.lookupScoped( from, transform.scope, transform.label ), transform.labelSpan );
  }
  for ( DriverRole const& role : module.driverRoles() )
  {
    if ( !role.label.empty() )
    {
      uses.add( symbols.lookupScoped( from, role.scope, role.label ), role.labelSpan );
    }
    if ( !role.window.empty() )
    {
      uses.add( symbols.lookupScoped( from, role.scope, role.window ), role.windowSpan );
    }
  }
  for ( CharsetDeclaration const& charset : module.charsets() )
  {
    if ( charset.base.has_value() )
    {
      uses.named( from, "", charset.base->name );
      if ( charset.base->mask != nullptr )
      {
        uses.walk( from, *charset.base->mask, none );
      }
    }
  }
}

/// Every place the Project file writes the name of a Symbol: the Label a Phase
/// starts at, a Label `load` calls, the Label a `transform` names a Section
/// by, and the Window a `panes` block is under.
void collectProject( Uses& uses, Merged const& build )
{
  GlobalSymbols const& symbols = build.symbols();
  Project const& project = build.project();
  PhaseGraph const& graph = project.phases;
  for ( std::uint32_t phase = 0; phase < graph.phases.size(); ++phase )
  {
    if ( std::optional<diag::SourceSpan> const span = graph.phases[phase].entrySpan; span.has_value() )
    {
      uses.add( entrySymbolOf( graph, PhaseIndex{ phase }, symbols ), *span );
    }
  }
  for ( LoadInit const& init : project.loadInits )
  {
    for ( ModuleIndex const module : project.loadNeeds )
    {
      uses.add( ownSymbol( symbols, module, init.name ), init.span );
    }
  }
  for ( TransformRequest const& request : project.transforms )
  {
    uses.add( ownSymbol( symbols, request.module, request.section ), request.span );
  }
  for ( Pane const& pane : project.target.panes )
  {
    uses.add( symbols.find( project.target.windows[pane.window.value].name ), pane.windowSpan );
  }
}

} // namespace

std::optional<SymbolSite> siteOf( diag::SourceManager const& sources, diag::SourceSpan span )
{
  if ( !span.begin.isValid() )
  {
    return std::nullopt;
  }
  std::optional<diag::FileId> const file = sources.fileContaining( span.begin );
  if ( !file.has_value() )
  {
    return std::nullopt;
  }
  std::uint32_t const base = sources.locationOf( *file, 0 ).rawOffset();
  return SymbolSite{ .file = *file, .offset = span.begin.rawOffset() - base, .length = span.length };
}

std::vector<SymbolFacts> symbolsOf( Merged const& build )
{
  diag::SourceManager const& sources = build.sources();

  std::vector<SymbolFacts> out;
  for ( std::uint32_t m = 0; m < build.modules().size(); ++m )
  {
    Module const& module = build.modules()[m];
    std::vector<Symbol> const& table = module.symbols().symbols();
    for ( std::uint32_t i = 0; i < table.size(); ++i )
    {
      Symbol const& symbol = table[i];
      SymbolFacts facts{ .name = std::string{ symbol.name },
                         .module = std::string{ module.name() },
                         .kind = std::string{ nameOf( symbol.kind ) },
                         .exported = symbol.exported,
                         .definition = siteOf( sources, symbol.definition ).value_or( SymbolSite{} ),
                         .uses = {},
                         .where = SymbolRef{ .module = ModuleIndex{ m }, .index = i } };
      if ( auto const* const label = std::get_if<LabelPosition>( &symbol.value );
           label != nullptr && label->section.value < module.sections().size() )
      {
        Section const& section = module.sections()[label->section.value];
        facts.section = module.displayNameOf( label->section );
        if ( std::optional<PaneIndex> const pane = section.pane(); pane.has_value() )
        {
          facts.pane = build.target().panes[pane->value].name;
        }
        if ( std::optional<UnitSetIndex> const set = section.unitSet(); set.has_value() )
        {
          facts.units = build.target().unitSets[set->value].name;
        }
      }
      out.push_back( std::move( facts ) );
    }
  }

  Uses uses{ build, out };
  for ( std::uint32_t m = 0; m < build.modules().size(); ++m )
  {
    collectModule( uses, build, ModuleIndex{ m } );
  }
  collectProject( uses, build );

  // In the order they are written: file by file as the build read them, and
  // through each file from its start.
  for ( SymbolFacts& facts : out )
  {
    std::ranges::sort( facts.uses,
                       []( SymbolSite const& a, SymbolSite const& b )
                       { return std::pair{ a.file.value, a.offset } < std::pair{ b.file.value, b.offset }; } );
  }
  return out;
}

void placeSymbols( std::vector<SymbolFacts>& facts, Merged const& build, ValueSource& known )
{
  GlobalSymbols const& symbols = build.symbols();
  for ( SymbolFacts& one : facts )
  {
    Module const& module = symbols.moduleAt( one.where.module );
    if ( one.where.index >= module.symbols().symbols().size() )
    {
      continue;
    }
    Symbol const& symbol = module.symbols().symbols()[one.where.index];
    if ( auto const* const label = std::get_if<LabelPosition>( &symbol.value );
         label != nullptr && label->section.value < module.sections().size() )
    {
      SectionRef const where{ .module = one.where.module, .section = label->section };
      std::optional<std::int64_t> const base = known.addressOf( where );
      std::optional<std::int64_t> const offset = known.offsetOf( where, label->chunk, std::nullopt );
      if ( base.has_value() && offset.has_value() )
      {
        one.address = *base + *offset;
      }
      if ( std::optional<PaneIndex> const pane = module.sections()[label->section.value].pane(); pane.has_value() )
      {
        one.paneState = static_cast<std::uint32_t>( known.stateOfPane( *pane ).value_or( 0 ) );
      }
    }
    else if ( auto const* const expression = std::get_if<syntax::ExpressionPtr>( &symbol.value );
              expression != nullptr && *expression != nullptr )
    {
      one.value = evaluate( build.sources(), symbols, &build.charsets(), one.where.module, **expression, known );
    }
  }
}

} // namespace nga::model
