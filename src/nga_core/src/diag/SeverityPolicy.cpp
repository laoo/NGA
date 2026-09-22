#include "nga/diag/SeverityPolicy.hpp"

namespace nga::diag
{

void SeverityPolicy::set( DiagnosticId id, SeverityOverride value )
{
  mOverrides.at( static_cast<std::size_t>( id ) ) = value;
}

void SeverityPolicy::clear( DiagnosticId id )
{
  mOverrides.at( static_cast<std::size_t>( id ) ).reset();
}

std::optional<Severity> SeverityPolicy::effectiveFor( DiagnosticId id ) const
{
  Severity const defaultSeverity = catalogEntryFor( id ).defaultSeverity;
  if ( defaultSeverity == Severity::NOTE )
  {
    return Severity::NOTE;
  }

  auto const& overrideValue = mOverrides.at( static_cast<std::size_t>( id ) );
  if ( !overrideValue.has_value() )
  {
    return defaultSeverity;
  }

  switch ( *overrideValue )
  {
  case SeverityOverride::DENY:
    return Severity::ERROR;
  case SeverityOverride::ALLOW:
    return Severity::WARNING;
  case SeverityOverride::OFF:
    return std::nullopt;
  }

  return defaultSeverity;
}

std::vector<DiagnosticId> diagnosticIdsFor( std::span<std::string const> codes, std::vector<std::string>& unknown )
{
  std::vector<DiagnosticId> found;
  found.reserve( codes.size() );
  for ( std::string const& code : codes )
  {
    if ( std::optional<DiagnosticId> const id = diagnosticIdForCode( code ); id.has_value() )
    {
      found.push_back( *id );
      continue;
    }
    unknown.push_back( code );
  }
  return found;
}

} // namespace nga::diag
