#pragma once

#include "nga/diag/Diagnostic.hpp"
#include "nga/diag/SeverityPolicy.hpp"
#include "nga/diag/SourceManager.hpp"

#include <cstddef>
#include <functional>
#include <span>
#include <vector>

namespace nga::diag
{

/// A diagnostic together with the severity the policy settled on.
struct Finding
{
  Diagnostic diagnostic;
  Severity severity = Severity::ERROR;
};

/// Collects findings for one Module. Not thread-safe by design: each Module
/// assembles into its own sink with no shared state and no locking, and the
/// sinks are merged afterwards in Project order.
class DiagnosticSink
{
public:
  explicit DiagnosticSink( SeverityPolicy const& policy ) : mPolicy( &policy ) {}

  /// Suppressed diagnostics are dropped here rather than at render time, so
  /// counts and the error limit reflect what the user actually asked for.
  void add( Diagnostic value );

  /// Appends another sink's findings. Call in Project order: the sort that
  /// follows is stable, so this order breaks ties that nothing else can.
  void merge( DiagnosticSink&& other );

  /// Drops every finding `keep` refuses, and recounts. What a `.off` in the
  /// source does at the end of a run, when every Step has reported.
  void retain( std::function<bool( Finding const& )> const& keep );

  /// Applies the ordering contract from docs/spec/diagnostics.md. Needs the
  /// SourceManager because positions are ordered by (file, offset in file)
  /// rather than by raw offset — see SourceManager::orderKeyFor.
  void sortForOutput( SourceManager const& sources );

  /// What a sink for another Module must be built with, so that one policy
  /// governs a whole run.
  [[nodiscard]] SeverityPolicy const& policy() const
  {
    return *mPolicy;
  }

  [[nodiscard]] std::span<Finding const> findings() const
  {
    return mFindings;
  }

  [[nodiscard]] std::size_t errorCount() const
  {
    return mErrorCount;
  }

  [[nodiscard]] std::size_t warningCount() const
  {
    return mWarningCount;
  }

  [[nodiscard]] bool hasErrors() const
  {
    return mErrorCount != 0;
  }

private:
  SeverityPolicy const* mPolicy;
  std::vector<Finding> mFindings;
  std::size_t mErrorCount = 0;
  std::size_t mWarningCount = 0;
};

} // namespace nga::diag
