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

} // namespace nga::model

namespace nga::model
{

std::string_view nameOf( Binding binding )
{
  switch ( binding )
  {
  case Binding::POINTER:
    return "pointer";
  case Binding::VECTOR:
    return "vector";
  }
  return "?";
}

} // namespace nga::model
