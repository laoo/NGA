#include "nga/Json.hpp"

namespace nga::json
{

void appendString( std::string& out, std::string_view text )
{
  out.push_back( '"' );
  for ( char const c : text )
  {
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
