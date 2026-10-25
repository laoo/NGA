#include "nga/model/Symbol.hpp"

#include <utility>

namespace nga::model
{

std::string_view nameOf( SymbolKind kind )
{
  switch ( kind )
  {
  case SymbolKind::LABEL:
    return "label";
  case SymbolKind::CONSTANT:
    return "constant";
  case SymbolKind::CHARSET:
    return "charset";
  case SymbolKind::SLOT:
    return "slot";
  case SymbolKind::REGION:
    return "region";
  case SymbolKind::WINDOW:
    return "window";
  case SymbolKind::PANE:
    return "pane";
  case SymbolKind::MACRO:
    return "macro";
  }
  return "symbol";
}

Symbol const* SymbolTable::find( std::string_view name ) const
{
  auto const entry = mIndex.find( name );
  return entry == mIndex.end() ? nullptr : &mSymbols[entry->second];
}

std::optional<std::uint32_t> SymbolTable::indexOf( std::string_view name ) const
{
  auto const entry = mIndex.find( name );
  return entry == mIndex.end() ? std::nullopt : std::optional{ entry->second };
}

Symbol const* SymbolTable::add( Symbol symbol )
{
  if ( Symbol const* const existing = find( symbol.name ); existing != nullptr )
  {
    return existing;
  }

  mIndex.emplace( symbol.name, static_cast<std::uint32_t>( mSymbols.size() ) );
  mSymbols.push_back( std::move( symbol ) );
  return nullptr;
}

bool SymbolTable::markExported( std::string_view name )
{
  auto const entry = mIndex.find( name );
  if ( entry == mIndex.end() )
  {
    return false;
  }
  mSymbols[entry->second].exported = true;
  return true;
}

std::string_view enclosingScopeOf( std::string_view scope )
{
  if ( scope.empty() )
  {
    return scope;
  }
  scope.remove_suffix( 1 );
  std::size_t const dot = scope.rfind( '.' );
  return dot == std::string_view::npos ? std::string_view{} : scope.substr( 0, dot + 1 );
}

} // namespace nga::model
