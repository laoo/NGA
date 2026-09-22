#include "nga/model/Suppressions.hpp"

#include <cstdint>
#include <map>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

namespace nga::model
{

namespace
{

/// A line of one file, which is what an inline expectation anchors to and
/// what a `.off` names: the statement below it, wherever on that line the
/// finding points.
using Line = std::pair<std::string_view, std::uint32_t>;

Line lineOf( diag::SourceManager const& sources, diag::SourceLocation location )
{
  diag::ExpandedLocation const expanded = sources.expand( location );
  return Line{ expanded.path, expanded.line };
}

} // namespace

void applySuppressions( diag::SourceManager const& sources,
                        std::span<Module const> modules,
                        diag::DiagnosticSink& sink )
{
  struct Armed
  {
    Suppression const* suppression;
    bool used = false;
  };

  std::map<std::tuple<std::string_view, std::uint32_t, diag::DiagnosticId>, std::vector<std::size_t>> byLine;
  std::vector<Armed> armed;
  for ( Module const& module : modules )
  {
    for ( Suppression const& suppression : module.suppressions() )
    {
      armed.push_back( Armed{ .suppression = &suppression, .used = false } );
      if ( suppression.statement.has_value() )
      {
        Line const line = lineOf( sources, *suppression.statement );
        byLine[std::tuple{ line.first, line.second, suppression.id }].push_back( armed.size() - 1 );
      }
    }
  }
  if ( armed.empty() )
  {
    return;
  }

  // Only a warning: an error is never silenced from the source, so a `deny`
  // from the Project or the command line outranks a `.off`, and the pipeline
  // stops where it would have stopped.
  sink.retain(
      [&]( diag::Finding const& finding )
      {
        if ( finding.severity != diag::Severity::WARNING || !finding.diagnostic.span.begin.isValid() )
        {
          return true;
        }
        Line const line = lineOf( sources, finding.diagnostic.span.begin );
        auto const found = byLine.find( std::tuple{ line.first, line.second, finding.diagnostic.id } );
        if ( found == byLine.end() )
        {
          return true;
        }
        for ( std::size_t const index : found->second )
        {
          armed[index].used = true;
        }
        return false;
      } );

  if ( sink.hasErrors() )
  {
    return;
  }
  for ( Armed const& one : armed )
  {
    if ( one.used )
    {
      continue;
    }
    sink.add( diag::diagnostic( diag::DiagnosticId::OFF_SILENCED_NOTHING )
                  .at( one.suppression->span.begin, one.suppression->span.length )
                  .arg( "code", one.suppression->code ) );
  }
}

} // namespace nga::model
