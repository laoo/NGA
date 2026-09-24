#include "nga/syntax/Literal.hpp"

#include <spdlog/fmt/fmt.h>

#include <array>
#include <cstdint>
#include <limits>

namespace nga::syntax
{

namespace
{

int valueOfDigit( char c )
{
  if ( c >= '0' && c <= '9' )
  {
    return c - '0';
  }
  if ( c >= 'a' && c <= 'f' )
  {
    return c - 'a' + 10;
  }
  if ( c >= 'A' && c <= 'F' )
  {
    return c - 'A' + 10;
  }
  return -1;
}

bool isQuote( char c )
{
  return c == '"' || c == '\'';
}

std::optional<std::int64_t> valueOfEscape( char c )
{
  switch ( c )
  {
  case '\\':
    return '\\';
  case '"':
    return '"';
  case '\'':
    return '\'';
  case 'n':
    return '\n';
  case 't':
    return '\t';
  case '0':
    return 0;
  default:
    return std::nullopt;
  }
}

} // namespace

std::optional<std::int64_t> numericValueOf( std::string_view text )
{
  int base = 10;
  if ( !text.empty() && ( text.front() == '$' || text.front() == '%' ) )
  {
    base = text.front() == '$' ? 16 : 2;
    text.remove_prefix( 1 );
  }

  std::int64_t value = 0;
  std::uint32_t digits = 0;
  for ( char const c : text )
  {
    if ( c == '_' )
    {
      continue;
    }

    int const digit = valueOfDigit( c );
    if ( digit < 0 || digit >= base )
    {
      return std::nullopt;
    }

    // Refused rather than wrapped: a literal nobody can represent is a mistake
    // in the source, not a number.
    if ( value > ( ( std::numeric_limits<std::int64_t>::max() - digit ) / base ) )
    {
      return std::nullopt;
    }
    value = ( value * base ) + digit;
    ++digits;
  }

  return digits == 0 ? std::nullopt : std::optional{ value };
}

Quoted quotedOf( std::string_view text )
{
  std::size_t const open = text.find_first_of( "\"'" );
  if ( open == std::string_view::npos )
  {
    return Quoted{};
  }

  std::string_view const charset = text.substr( 0, open );
  std::string_view rest = text.substr( open + 1 );
  if ( !rest.empty() && isQuote( rest.back() ) )
  {
    rest.remove_suffix( 1 );
  }
  return Quoted{ .charset = charset, .body = rest };
}

std::uint32_t characterCountOf( std::string_view body )
{
  std::uint32_t count = 0;
  for ( std::size_t at = 0; at < body.size(); )
  {
    if ( body[at] == '\\' )
    {
      at += 2;
    }
    else
    {
      // A continuation byte never starts a character, so one code point counts
      // once however many bytes it took to write.
      ++at;
      while ( at < body.size() && ( static_cast<unsigned char>( body[at] ) & 0xC0U ) == 0x80U )
      {
        ++at;
      }
    }
    ++count;
  }
  return count;
}

std::vector<char32_t> codePointsOf( std::string_view body )
{
  std::vector<char32_t> points;
  for ( std::size_t at = 0; at < body.size(); )
  {
    if ( body[at] == '\\' && at + 1 < body.size() )
    {
      std::optional<std::int64_t> const escaped = valueOfEscape( body[at + 1] );
      points.push_back( static_cast<char32_t>( escaped.value_or( body[at + 1] ) ) );
      at += 2;
      continue;
    }

    auto const lead = static_cast<unsigned char>( body[at] );
    std::uint32_t extra = 0;
    char32_t point = lead;
    if ( ( lead & 0x80U ) != 0 )
    {
      // How many continuation bytes follow, from the leading byte alone. The
      // lexer has already refused everything this would misread.
      if ( ( lead & 0xE0U ) == 0xC0U )
      {
        extra = 1;
      }
      else if ( ( lead & 0xF0U ) == 0xE0U )
      {
        extra = 2;
      }
      else
      {
        extra = 3;
      }
      point = lead & ( 0x7FU >> ( extra + 1 ) );
    }
    ++at;
    for ( std::uint32_t more = 0; more < extra && at < body.size(); ++more, ++at )
    {
      point = ( point << 6U ) | ( static_cast<unsigned char>( body[at] ) & 0x3FU );
    }
    points.push_back( point );
  }
  return points;
}

std::string displayOf( char32_t codePoint )
{
  auto const value = static_cast<std::uint32_t>( codePoint );

  // What the language spells, spelled that way; every other control character
  // by its number, as `NGA7002` names one the C lexer meets.
  switch ( value )
  {
  case 0x00U:
    return "\\0";
  case 0x09U:
    return "\\t";
  case 0x0AU:
    return "\\n";
  default:
    break;
  }
  // C0 and C1 controls, DEL, and the two separators Unicode reads as line
  // breaks: each of them would end or displace the line the finding stands on.
  if ( value < 0x20U || value == 0x7FU || ( value >= 0x80U && value <= 0x9FU ) || value == 0x2028U || value == 0x2029U )
  {
    return fmt::format( "U+{:04X}", value );
  }

  std::string text;
  if ( value < 0x80U )
  {
    text.push_back( static_cast<char>( value ) );
  }
  else if ( value < 0x800U )
  {
    text.push_back( static_cast<char>( 0xC0U | ( value >> 6U ) ) );
    text.push_back( static_cast<char>( 0x80U | ( value & 0x3FU ) ) );
  }
  else if ( value < 0x10000U )
  {
    text.push_back( static_cast<char>( 0xE0U | ( value >> 12U ) ) );
    text.push_back( static_cast<char>( 0x80U | ( ( value >> 6U ) & 0x3FU ) ) );
    text.push_back( static_cast<char>( 0x80U | ( value & 0x3FU ) ) );
  }
  else
  {
    text.push_back( static_cast<char>( 0xF0U | ( value >> 18U ) ) );
    text.push_back( static_cast<char>( 0x80U | ( ( value >> 12U ) & 0x3FU ) ) );
    text.push_back( static_cast<char>( 0x80U | ( ( value >> 6U ) & 0x3FU ) ) );
    text.push_back( static_cast<char>( 0x80U | ( value & 0x3FU ) ) );
  }
  return text;
}

std::optional<std::int64_t> plainCharacterValueOf( std::string_view text )
{
  Quoted const quoted = quotedOf( text );
  if ( !quoted.charset.empty() || quoted.body.empty() )
  {
    return std::nullopt;
  }

  if ( quoted.body.front() == '\\' )
  {
    return quoted.body.size() == 2 ? valueOfEscape( quoted.body[1] ) : std::nullopt;
  }
  return quoted.body.size() == 1 ? std::optional<std::int64_t>{ static_cast<unsigned char>( quoted.body.front() ) }
                                 : std::nullopt;
}

std::optional<std::vector<std::uint8_t>> plainBytesOf( std::string_view text )
{
  Quoted const quoted = quotedOf( text );
  if ( !quoted.charset.empty() )
  {
    return std::nullopt;
  }

  std::vector<std::uint8_t> bytes;
  bytes.reserve( quoted.body.size() );
  for ( std::size_t at = 0; at < quoted.body.size(); ++at )
  {
    if ( quoted.body[at] != '\\' || at + 1 >= quoted.body.size() )
    {
      bytes.push_back( static_cast<std::uint8_t>( quoted.body[at] ) );
      continue;
    }

    std::optional<std::int64_t> const escaped = valueOfEscape( quoted.body[at + 1] );
    if ( !escaped.has_value() )
    {
      return std::nullopt;
    }
    bytes.push_back( static_cast<std::uint8_t>( *escaped ) );
    ++at;
  }
  return bytes;
}

std::uint32_t utf8LengthAt( std::string_view text, std::uint32_t pos )
{
  auto const lead = static_cast<unsigned char>( text[pos] );

  if ( lead < 0x80U )
  {
    return 1;
  }

  std::uint32_t length = 0;
  std::uint32_t code = 0;
  if ( ( lead & 0xE0U ) == 0xC0U )
  {
    length = 2;
    code = lead & 0x1FU;
  }
  else if ( ( lead & 0xF0U ) == 0xE0U )
  {
    length = 3;
    code = lead & 0x0FU;
  }
  else if ( ( lead & 0xF8U ) == 0xF0U )
  {
    length = 4;
    code = lead & 0x07U;
  }
  else
  {
    return 0;
  }

  if ( pos + length > text.size() )
  {
    return 0;
  }
  for ( std::uint32_t i = 1; i < length; ++i )
  {
    auto const continuation = static_cast<unsigned char>( text[pos + i] );
    if ( ( continuation & 0xC0U ) != 0x80U )
    {
      return 0;
    }
    code = ( code << 6U ) | ( continuation & 0x3FU );
  }

  static constexpr std::array<std::uint32_t, 5> SHORTEST = { 0, 0, 0x80, 0x800, 0x10000 };
  if ( code < SHORTEST.at( length ) || code > 0x10FFFFU || ( code >= 0xD800U && code <= 0xDFFFU ) )
  {
    return 0;
  }
  return length;
}

} // namespace nga::syntax
