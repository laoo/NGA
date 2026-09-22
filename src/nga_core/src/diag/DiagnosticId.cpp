#include "nga/diag/DiagnosticId.hpp"

#include <array>
#include <cstddef>

namespace nga::diag
{

namespace
{

// The code strings are spelled out rather than assembled from the number,
// because they appear in user output and in tests and must be greppable.
constexpr std::array<CatalogEntry, static_cast<std::size_t>( DiagnosticId::COUNT )> CATALOG = {
#define NGA_DIAGNOSTIC_ROW( name, number, severity, text )                                                             \
  CatalogEntry{ "NGA" #number, #name, Severity::severity, text },
  NGA_DIAGNOSTIC_CATALOG( NGA_DIAGNOSTIC_ROW )
#undef NGA_DIAGNOSTIC_ROW
};

} // namespace

CatalogEntry const& catalogEntryFor( DiagnosticId id )
{
  return CATALOG.at( static_cast<std::size_t>( id ) );
}

std::optional<DiagnosticId> diagnosticIdForCode( std::string_view code )
{
  for ( std::size_t i = 0; i < CATALOG.size(); ++i )
  {
    if ( CATALOG[i].code == code )
    {
      return static_cast<DiagnosticId>( i );
    }
  }
  return std::nullopt;
}

} // namespace nga::diag
