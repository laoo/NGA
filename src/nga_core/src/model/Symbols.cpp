#include "nga/model/Symbols.hpp"

#include "nga/model/Merge.hpp"
#include "nga/model/Module.hpp"
#include "nga/model/Symbol.hpp"
#include "nga/syntax/Expression.hpp"

#include <cstddef>
#include <map>
#include <optional>

namespace nga::model
{

namespace
{

/// Where in its own file a span stands, which is what a consumer can use: the
/// SourceManager's offsets are positions in a space it lays every file out in.
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

/// Every name one expression writes, with where it resolves to.
void walk( GlobalSymbols const& symbols,
           diag::SourceManager const& sources,
           ModuleIndex from,
           syntax::Expression const& node,
           std::map<std::pair<std::uint32_t, std::uint32_t>, SymbolSite>& found,
           std::vector<std::pair<SymbolRef, SymbolSite>>& uses )
{
  switch ( node.kind )
  {
  case syntax::ExpressionKind::NAME:
  {
    std::optional<SymbolRef> const where = symbols.resolveText( from, node, sources.textOf( node.token.span() ) );
    if ( where.has_value() )
    {
      if ( std::optional<SymbolSite> const site = siteOf( sources, node.token.span() ); site.has_value() )
      {
        uses.emplace_back( *where, *site );
      }
    }
    return;
  }
  case syntax::ExpressionKind::ATTRIBUTE:
  {
    // `level.levelMap` is one name before it is an attribute of anything, so
    // it is tried whole first -- the same order the model resolves it in.
    if ( std::optional<std::string> const dotted = syntax::dottedNameOf( sources, node ); dotted.has_value() )
    {
      if ( std::optional<SymbolRef> const where = symbols.resolveText( from, syntax::leftmostOf( node ), *dotted );
           where.has_value() )
      {
        if ( std::optional<SymbolSite> const site = siteOf( sources, node.span ); site.has_value() )
        {
          uses.emplace_back( *where, *site );
        }
        return;
      }
    }
    break;
  }
  default:
    break;
  }

  // A node holds at most two: `left` alone for a unary or an attribute's
  // receiver, both for a binary.
  if ( node.left != nullptr )
  {
    walk( symbols, sources, from, *node.left, found, uses );
  }
  if ( node.right != nullptr )
  {
    walk( symbols, sources, from, *node.right, found, uses );
  }
}

} // namespace

std::vector<SymbolFacts> symbolsOf( Merged const& build )
{
  diag::SourceManager const& sources = build.sources();
  GlobalSymbols const& symbols = build.symbols();

  std::vector<SymbolFacts> out;
  std::map<std::pair<std::uint32_t, std::uint32_t>, std::size_t> at;

  for ( std::size_t m = 0; m < build.modules().size(); ++m )
  {
    Module const& module = build.modules()[m];
    std::vector<Symbol> const& table = module.symbols().symbols();
    for ( std::size_t i = 0; i < table.size(); ++i )
    {
      Symbol const& symbol = table[i];
      std::optional<SymbolSite> const site = siteOf( sources, symbol.definition );
      at.emplace( std::pair{ static_cast<std::uint32_t>( m ), static_cast<std::uint32_t>( i ) }, out.size() );
      out.push_back( SymbolFacts{ .name = std::string{ symbol.name },
                                  .module = std::string{ module.name() },
                                  .kind = std::string{ nameOf( symbol.kind ) },
                                  .definition = site.value_or( SymbolSite{} ),
                                  .uses = {} } );
    }
  }

  std::vector<std::pair<SymbolRef, SymbolSite>> uses;
  std::map<std::pair<std::uint32_t, std::uint32_t>, SymbolSite> unused;
  for ( std::size_t m = 0; m < build.modules().size(); ++m )
  {
    Module const& module = build.modules()[m];
    ModuleIndex const from{ static_cast<std::uint32_t>( m ) };
    for ( Section const& section : module.sections() )
    {
      for ( Chunk const& chunk : section.chunks() )
      {
        for ( syntax::ExpressionPtr const& item : section.itemsOf( chunk ) )
        {
          if ( item != nullptr )
          {
            walk( symbols, sources, from, *item, unused, uses );
          }
        }
      }
    }
  }

  for ( auto const& [where, site] : uses )
  {
    auto const found = at.find( { where.module.value, where.index } );
    if ( found != at.end() )
    {
      out[found->second].uses.push_back( site );
    }
  }

  return out;
}

} // namespace nga::model
