#include "nga/diag/DiagnosticSink.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <tuple>
#include <utility>
#include <vector>

namespace nga::diag
{

void DiagnosticSink::add( Diagnostic value )
{
  auto const severity = mPolicy->effectiveFor( value.id );
  if ( !severity.has_value() )
  {
    return;
  }

  switch ( *severity )
  {
  case Severity::ERROR:
    ++mErrorCount;
    break;
  case Severity::WARNING:
    ++mWarningCount;
    break;
  case Severity::NOTE:
    break;
  }

  mFindings.push_back( Finding{ .diagnostic = std::move( value ), .severity = *severity } );
}

void DiagnosticSink::merge( DiagnosticSink&& other )
{
  mFindings.insert( mFindings.end(),
                    std::make_move_iterator( other.mFindings.begin() ),
                    std::make_move_iterator( other.mFindings.end() ) );
  other.mFindings.clear();

  mErrorCount += std::exchange( other.mErrorCount, 0 );
  mWarningCount += std::exchange( other.mWarningCount, 0 );
}

void DiagnosticSink::retain( std::function<bool( Finding const& )> const& keep )
{
  std::erase_if( mFindings, [&keep]( Finding const& finding ) { return !keep( finding ); } );
  mErrorCount = 0;
  mWarningCount = 0;
  for ( Finding const& finding : mFindings )
  {
    mErrorCount += finding.severity == Severity::ERROR ? 1 : 0;
    mWarningCount += finding.severity == Severity::WARNING ? 1 : 0;
  }
}

void DiagnosticSink::sortForOutput( SourceManager const& sources )
{
  // Positions are keyed once per finding rather than inside the comparator:
  // deriving the key means a binary search over the file table, and a sort
  // performs O(n log n) comparisons.
  struct Keyed
  {
    std::uint64_t position;
    bool located;
    std::size_t index;
  };

  std::vector<Keyed> keys;
  keys.reserve( mFindings.size() );
  for ( std::size_t i = 0; i < mFindings.size(); ++i )
  {
    SourceLocation const begin = mFindings[i].diagnostic.span.begin;
    keys.push_back( Keyed{ .position = sources.orderKeyFor( begin ), .located = begin.isValid(), .index = i } );
  }

  // Located findings first, ordered by position; then whole-program findings,
  // ordered by identifier and sort key. The sort is stable, so anything still
  // tied keeps the Project order that merge() established — never completion
  // order, and never a pointer or hash.
  std::ranges::stable_sort( keys,
                            [this]( Keyed const& left, Keyed const& right )
                            {
                              if ( left.located != right.located )
                              {
                                return left.located;
                              }
                              if ( left.located )
                              {
                                return left.position < right.position;
                              }

                              Diagnostic const& leftValue = mFindings[left.index].diagnostic;
                              Diagnostic const& rightValue = mFindings[right.index].diagnostic;
                              return std::tie( leftValue.id, leftValue.sortKey ) <
                                     std::tie( rightValue.id, rightValue.sortKey );
                            } );

  std::vector<Finding> sorted;
  sorted.reserve( mFindings.size() );
  for ( Keyed const& key : keys )
  {
    sorted.push_back( std::move( mFindings[key.index] ) );
  }
  mFindings = std::move( sorted );
}

} // namespace nga::diag
