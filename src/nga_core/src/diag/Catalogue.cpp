#include "nga/diag/Catalogue.hpp"

#include "nga/Json.hpp"
#include "nga/Version.hpp"

#include <array>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <string>

namespace nga::diag
{

namespace
{

struct Range
{
  std::uint16_t first;
  std::uint16_t last;
  std::string_view step;
};

// Where each block of numbers belongs, which is the table of
// docs/spec/diagnostics.md as data. A new diagnostic takes a number from the
// block of the Step that raises it, and a block that is not covered here is a
// number nobody can place -- which `catalogue` in the suite refuses.
//
// `NGA524x` is inside the Place block and is not raised by a Step at all: it
// is the layout verifier re-reading what Place produced, and a finding of it
// means the tool is wrong rather than the program. It is listed before the
// range that contains it, and the search takes the first that matches.
constexpr std::array<Range, 12> RANGES = {
  Range{ .first = 5240, .last = 5249, .step = "Layout verification" },
  Range{ .first = 0, .last = 99, .step = "Source lexing" },
  Range{ .first = 100, .last = 999, .step = "Source parsing" },
  Range{ .first = 1000, .last = 1999, .step = "Project file and run configuration" },
  Range{ .first = 2000, .last = 2999, .step = "Symbols, Merge, duplicates and Slots" },
  Range{ .first = 3000, .last = 3999, .step = "Context, Requirements and interrupt handlers" },
  Range{ .first = 4000, .last = 4999, .step = "Prune and Trace" },
  Range{ .first = 5000, .last = 5999, .step = "Size and Place" },
  Range{ .first = 6000, .last = 6999, .step = "Patch, Emit, compression and the Container" },
  Range{ .first = 7000, .last = 7099, .step = "Lexing a `.ngc` Module" },
  Range{ .first = 7100, .last = 7199, .step = "Parsing a `.ngc` Module" },
  Range{ .first = 7200, .last = 7999, .step = "Compiling a `.ngc` Module" },
};

std::uint16_t numberOf( std::string_view code )
{
  // "NGA2412" -- the four digits behind the three letters.
  std::uint16_t value = 0;
  std::from_chars( code.data() + 3, code.data() + code.size(), value );
  return value;
}

std::string_view severityName( Severity value )
{
  switch ( value )
  {
  case Severity::ERROR:
    return "error";
  case Severity::WARNING:
    return "warning";
  case Severity::NOTE:
    return "note";
  }
  return "error";
}

} // namespace

std::string_view stepOf( DiagnosticId id )
{
  std::uint16_t const number = numberOf( catalogEntryFor( id ).code );
  for ( Range const& range : RANGES )
  {
    if ( number >= range.first && number <= range.last )
    {
      return range.step;
    }
  }
  return {};
}

std::string renderCatalogueJson()
{
  std::string out;
  out.append( "{\n  \"schema\": 1,\n  \"nga\": " );
  json::appendString( out, versionString() );
  out.append( ",\n  \"diagnostics\": [\n" );

  for ( std::size_t i = 0; i < static_cast<std::size_t>( DiagnosticId::COUNT ); ++i )
  {
    auto const id = static_cast<DiagnosticId>( i );
    CatalogEntry const& entry = catalogEntryFor( id );

    out.append( "    { \"code\": " );
    json::appendString( out, entry.code );
    out.append( ", \"name\": " );
    json::appendString( out, entry.name );
    out.append( ", \"severity\": " );
    json::appendString( out, severityName( entry.defaultSeverity ) );
    out.append( ", \"step\": " );
    json::appendString( out, stepOf( id ) );
    out.append( ", \"template\": " );
    json::appendString( out, entry.messageTemplate );
    out.append( " }" );
    if ( i + 1 < static_cast<std::size_t>( DiagnosticId::COUNT ) )
    {
      out.push_back( ',' );
    }
    out.push_back( '\n' );
  }

  out.append( "  ]\n}\n" );
  return out;
}

} // namespace nga::diag
