#include "nga/Json.hpp"

#include <cstddef>

namespace nga::json
{

namespace
{

/// The length of the well-formed UTF-8 sequence at `at`, or zero: an overlong
/// spelling, a surrogate and anything past U+10FFFF are none, as the lexers
/// hold a source to.
std::size_t sequenceAt( std::string_view text, std::size_t at )
{
  auto const byte = [text]( std::size_t index ) { return static_cast<unsigned char>( text[index] ); };
  unsigned char const lead = byte( at );
  std::size_t length = 0;
  char32_t point = 0;
  char32_t least = 0;
  if ( ( lead & 0xE0U ) == 0xC0U )
  {
    length = 2;
    point = lead & 0x1FU;
    least = 0x80;
  }
  else if ( ( lead & 0xF0U ) == 0xE0U )
  {
    length = 3;
    point = lead & 0x0FU;
    least = 0x800;
  }
  else if ( ( lead & 0xF8U ) == 0xF0U )
  {
    length = 4;
    point = lead & 0x07U;
    least = 0x10000;
  }
  if ( length == 0 || at + length > text.size() )
  {
    return 0;
  }
  for ( std::size_t index = 1; index < length; ++index )
  {
    if ( ( byte( at + index ) & 0xC0U ) != 0x80U )
    {
      return 0;
    }
    point = ( point << 6U ) | ( byte( at + index ) & 0x3FU );
  }
  bool const surrogate = point >= 0xD800 && point <= 0xDFFF;
  return point < least || point > 0x10FFFF || surrogate ? 0 : length;
}

} // namespace

void appendString( std::string& out, std::string_view text )
{
  out.push_back( '"' );
  for ( std::size_t at = 0; at < text.size(); ++at )
  {
    char const c = text[at];
    // A document of JSON is UTF-8, so what is not is written as the
    // replacement character rather than passed on to break it.
    if ( static_cast<unsigned char>( c ) >= 0x80 )
    {
      std::size_t const length = sequenceAt( text, at );
      if ( length == 0 )
      {
        out.append( "\\ufffd" );
        continue;
      }
      out.append( text.substr( at, length ) );
      at += length - 1;
      continue;
    }
    switch ( c )
    {
    case '"':
      out.append( R"(\")" );
      break;
    case '\\':
      out.append( R"(\\)" );
      break;
    case '\n':
      out.append( "\\n" );
      break;
    case '\r':
      out.append( "\\r" );
      break;
    case '\t':
      out.append( "\\t" );
      break;
    default:
      if ( static_cast<unsigned char>( c ) < 0x20 )
      {
        static constexpr std::string_view DIGITS = "0123456789abcdef";
        out.append( "\\u00" );
        out.push_back( DIGITS[( static_cast<unsigned char>( c ) >> 4 ) & 0xF] );
        out.push_back( DIGITS[static_cast<unsigned char>( c ) & 0xF] );
      }
      else
      {
        out.push_back( c );
      }
      break;
    }
  }
  out.push_back( '"' );
}

} // namespace nga::json
