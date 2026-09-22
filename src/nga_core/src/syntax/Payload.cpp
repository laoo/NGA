#include "nga/syntax/Payload.hpp"

#include <algorithm>
#include <cstddef>

namespace nga::syntax
{

namespace
{

/// One accepted character: what it is worth, where it stood, and whether it was
/// base64's padding, which is a position rule rather than an alphabet one.
struct Accepted
{
  std::uint8_t value = 0;
  std::uint32_t offset = 0;
  bool padding = false;
};

bool isBlank( char c )
{
  return c == ' ' || c == '\t';
}

/// What the character is worth in `encoding`, or -1 where the alphabet has no
/// such character. `=` answers 0 and is told apart by the caller.
int valueOf( Payload encoding, char c )
{
  switch ( encoding )
  {
  case Payload::HEX:
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

  case Payload::BINARY:
    return c == '0' || c == '1' ? c - '0' : -1;

  case Payload::BASE64:
    if ( c >= 'A' && c <= 'Z' )
    {
      return c - 'A';
    }
    if ( c >= 'a' && c <= 'z' )
    {
      return c - 'a' + 26;
    }
    if ( c >= '0' && c <= '9' )
    {
      return c - '0' + 52;
    }
    if ( c == '+' )
    {
      return 62;
    }
    if ( c == '/' )
    {
      return 63;
    }
    return c == '=' ? 0 : -1;
  }
  return -1;
}

/// How many bytes this lead byte begins. The lexer has already refused
/// ill-formed UTF-8 in a literal, so a continuation byte here is one the caller
/// walked into and is counted as one character of its own.
std::uint32_t utf8LengthOf( unsigned char lead )
{
  if ( lead < 0x80U )
  {
    return 1;
  }
  if ( ( lead & 0xE0U ) == 0xC0U )
  {
    return 2;
  }
  if ( ( lead & 0xF0U ) == 0xE0U )
  {
    return 3;
  }
  if ( ( lead & 0xF8U ) == 0xF0U )
  {
    return 4;
  }
  return 1;
}

void decodeGroups( Payload encoding, std::vector<Accepted> const& accepted, DecodedPayload& result )
{
  std::uint32_t const group = charactersPerGroup( encoding );
  std::size_t const whole = accepted.size() / group * group;

  for ( std::size_t at = 0; at + group <= whole; at += group )
  {
    switch ( encoding )
    {
    case Payload::HEX:
      result.bytes.push_back( static_cast<std::uint8_t>( static_cast<unsigned>( accepted[at].value ) << 4U |
                                                         static_cast<unsigned>( accepted[at + 1].value ) ) );
      break;

    case Payload::BINARY:
    {
      std::uint8_t byte = 0;
      for ( std::size_t bit = 0; bit < group; ++bit )
      {
        byte = static_cast<std::uint8_t>( byte << 1U | accepted[at + bit].value );
      }
      result.bytes.push_back( byte );
      break;
    }

    case Payload::BASE64:
    {
      std::uint32_t const bits = static_cast<std::uint32_t>( accepted[at].value ) << 18U |
                                 static_cast<std::uint32_t>( accepted[at + 1].value ) << 12U |
                                 static_cast<std::uint32_t>( accepted[at + 2].value ) << 6U |
                                 static_cast<std::uint32_t>( accepted[at + 3].value );

      // Padding says how much of the last group is data: `QQ==` is one byte and
      // `QUE=` is two, which is the one place a group yields fewer than three.
      std::uint32_t const pads = static_cast<std::uint32_t>( accepted[at + 2].padding ) +
                                 static_cast<std::uint32_t>( accepted[at + 3].padding );

      result.bytes.push_back( static_cast<std::uint8_t>( bits >> 16U & 0xFFU ) );
      if ( pads < 2 )
      {
        result.bytes.push_back( static_cast<std::uint8_t>( bits >> 8U & 0xFFU ) );
      }
      if ( pads < 1 )
      {
        result.bytes.push_back( static_cast<std::uint8_t>( bits & 0xFFU ) );
      }
      break;
    }
    }
  }
}

/// Padding closes a payload and stands nowhere else: at most two characters,
/// and only the last two of the last whole group.
void checkPadding( std::vector<Accepted> const& accepted, DecodedPayload& result )
{
  // Where the payload does not divide into whole bytes, the length is the one
  // thing wrong with it: padding in the last group of a group that is not there
  // is the same mistake said twice.
  if ( !result.wholeBytes )
  {
    return;
  }

  for ( std::size_t at = 0; at < accepted.size(); ++at )
  {
    if ( !accepted[at].padding )
    {
      continue;
    }

    // The final character, or the one before it with the final one padding too:
    // `QQ==` and `QUE=` close a payload, `QQ=A` does not.
    bool const closesPayload = at + 1 == accepted.size() || ( at + 2 == accepted.size() && accepted.back().padding );
    if ( closesPayload )
    {
      continue;
    }

    result.faults.push_back(
        PayloadFault{ .offset = accepted[at].offset, .length = 1, .character = "=", .padding = true } );
  }
}

} // namespace

std::string_view nameOf( Payload encoding )
{
  switch ( encoding )
  {
  case Payload::HEX:
    return "hex";
  case Payload::BINARY:
    return "binary";
  case Payload::BASE64:
    return "base64";
  }
  return "hex";
}

std::uint32_t charactersPerGroup( Payload encoding )
{
  switch ( encoding )
  {
  case Payload::HEX:
    return 2;
  case Payload::BINARY:
    return 8;
  case Payload::BASE64:
    return 4;
  }
  return 2;
}

DecodedPayload decodePayload( Payload encoding, std::string_view body )
{
  DecodedPayload result;
  std::vector<Accepted> accepted;

  for ( std::size_t at = 0; at < body.size(); )
  {
    char const c = body[at];

    if ( isBlank( c ) )
    {
      ++at;
      continue;
    }

    int const value = valueOf( encoding, c );
    if ( value >= 0 )
    {
      accepted.push_back( Accepted{
          .value = static_cast<std::uint8_t>( value ),
          .offset = static_cast<std::uint32_t>( at ),
          .padding = encoding == Payload::BASE64 && c == '=',
      } );
      ++at;
      continue;
    }

    // An escape is one character of the literal and is no character of any
    // alphabet, so it is one fault of two bytes rather than two faults.
    std::uint32_t const length =
        c == '\\' && at + 1 < body.size() ? 2 : utf8LengthOf( static_cast<unsigned char>( c ) );
    std::uint32_t const span = static_cast<std::uint32_t>( std::min<std::size_t>( length, body.size() - at ) );

    result.faults.push_back( PayloadFault{
        .offset = static_cast<std::uint32_t>( at ),
        .length = span,
        .character = std::string{ body.substr( at, span ) },
        .padding = false,
    } );
    at += span;
  }

  result.characters = static_cast<std::uint32_t>( accepted.size() );
  result.wholeBytes = accepted.size() % charactersPerGroup( encoding ) == 0;

  checkPadding( accepted, result );
  decodeGroups( encoding, accepted, result );

  return result;
}

} // namespace nga::syntax
