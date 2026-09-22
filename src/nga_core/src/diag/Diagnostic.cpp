#include "nga/diag/Diagnostic.hpp"

#include <algorithm>
#include <string_view>
#include <utility>

namespace nga::diag
{

namespace
{

std::string groupDigits( std::int64_t value )
{
  std::string const digits = std::to_string( value < 0 ? -value : value );

  std::string grouped;
  grouped.reserve( digits.size() + ( digits.size() / 3 ) );
  for ( std::size_t i = 0; i < digits.size(); ++i )
  {
    if ( i != 0 && ( digits.size() - i ) % 3 == 0 )
    {
      grouped.push_back( ',' );
    }
    grouped.push_back( digits[i] );
  }

  return value < 0 ? "-" + grouped : grouped;
}

std::string toHex( std::int64_t value )
{
  static constexpr std::string_view DIGITS = "0123456789ABCDEF";

  auto bits = static_cast<std::uint64_t>( value );
  std::string text;
  do
  {
    text.push_back( DIGITS[bits & 0xFU] );
    bits >>= 4;
  } while ( bits != 0 );

  // 6502 addresses read as $D301, not $d301 and not 0xD301.
  if ( text.size() < 2 )
  {
    text.push_back( '0' );
  }
  text.push_back( '$' );
  std::ranges::reverse( text );
  return text;
}

std::string formatValue( ArgumentValue const& value, std::string_view hint )
{
  if ( auto const* text = std::get_if<std::string>( &value ) )
  {
    return *text;
  }
  if ( auto const* flag = std::get_if<bool>( &value ) )
  {
    return *flag ? "true" : "false";
  }

  auto const number = std::get<std::int64_t>( value );
  if ( hint == "hex" )
  {
    return toHex( number );
  }
  if ( hint == "n" )
  {
    return groupDigits( number );
  }
  return std::to_string( number );
}

} // namespace

Diagnostic diagnostic( DiagnosticId id )
{
  Diagnostic value;
  value.id = id;
  return value;
}

Diagnostic Diagnostic::at( SourceLocation location, std::uint32_t length ) &&
{
  span = SourceSpan{ .begin = location, .length = length };
  return std::move( *this );
}

Diagnostic Diagnostic::arg( std::string name, ArgumentValue value ) &&
{
  arguments.push_back( Argument{ .name = std::move( name ), .value = std::move( value ) } );
  return std::move( *this );
}

Diagnostic Diagnostic::note( Diagnostic child ) &&
{
  notes.push_back( std::move( child ) );
  return std::move( *this );
}

Diagnostic Diagnostic::sortedBy( std::string key ) &&
{
  sortKey = std::move( key );
  return std::move( *this );
}

std::string renderMessage( std::string_view messageTemplate, std::span<Argument const> arguments )
{
  std::string output;
  output.reserve( messageTemplate.size() + 32 );

  for ( std::size_t i = 0; i < messageTemplate.size(); ++i )
  {
    char const c = messageTemplate[i];

    if ( c == '{' && i + 1 < messageTemplate.size() && messageTemplate[i + 1] == '{' )
    {
      output.push_back( '{' );
      ++i;
      continue;
    }
    if ( c == '}' && i + 1 < messageTemplate.size() && messageTemplate[i + 1] == '}' )
    {
      output.push_back( '}' );
      ++i;
      continue;
    }
    if ( c != '{' )
    {
      output.push_back( c );
      continue;
    }

    std::size_t const close = messageTemplate.find( '}', i );
    if ( close == std::string_view::npos )
    {
      output.append( messageTemplate.substr( i ) );
      break;
    }

    std::string_view field = messageTemplate.substr( i + 1, close - i - 1 );
    std::string_view hint;
    if ( std::size_t const colon = field.find( ':' ); colon != std::string_view::npos )
    {
      hint = field.substr( colon + 1 );
      field = field.substr( 0, colon );
    }

    auto const found = std::ranges::find( arguments, field, &Argument::name );
    if ( found == arguments.end() )
    {
      output.append( "<missing " ).append( field ).push_back( '>' );
    }
    else
    {
      output.append( formatValue( found->value, hint ) );
    }

    i = close;
  }

  return output;
}

std::string renderMessage( Diagnostic const& value )
{
  return renderMessage( catalogEntryFor( value.id ).messageTemplate, value.arguments );
}

} // namespace nga::diag
