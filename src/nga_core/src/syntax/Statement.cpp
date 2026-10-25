#include "nga/syntax/Statement.hpp"

namespace nga::syntax
{

std::string_view nameOf( PlacementClass placement )
{
  switch ( placement )
  {
  case PlacementClass::ZEROPAGE:
    return "zeropage";
  case PlacementClass::ABSOLUTE:
    return "absolute";
  }
  return "absolute";
}

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

std::string spellingOf( DeclaredType const& type )
{
  switch ( type.kind )
  {
  case DeclaredType::Kind::I8:
    return "i8";
  case DeclaredType::Kind::U16:
    return "u16";
  case DeclaredType::Kind::I16:
    return "i16";
  case DeclaredType::Kind::BOOL:
    return "bool";
  case DeclaredType::Kind::BYTES:
    return "u8[" + std::to_string( type.count ) + "]";
  case DeclaredType::Kind::U8:
    break;
  }
  return "u8";
}

} // namespace nga::syntax
