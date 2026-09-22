#pragma once

#include "nga/diag/DiagnosticId.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace nga::diag
{

enum class SeverityOverride : std::uint8_t
{
  DENY,  ///< raise to an error
  ALLOW, ///< lower to a warning
  OFF,   ///< suppress entirely
};

/// Per-identifier control over how seriously a finding is taken.
///
/// Settable from the command line and from the Project file, deliberately: what
/// a project is willing to tolerate is a property of that project, not of
/// whoever happens to run the build.
class SeverityPolicy
{
public:
  void set( DiagnosticId id, SeverityOverride value );
  void clear( DiagnosticId id );

  /// Effective severity, or nothing when the diagnostic is suppressed.
  /// Notes are never overridden: they exist only as part of a parent.
  [[nodiscard]] std::optional<Severity> effectiveFor( DiagnosticId id ) const;

private:
  static constexpr std::size_t SLOT_COUNT = static_cast<std::size_t>( DiagnosticId::COUNT );

  std::array<std::optional<SeverityOverride>, SLOT_COUNT> mOverrides{};
};

/// The identifiers a list of user-visible codes names.
///
/// A code naming no diagnostic is appended to `unknown` rather than skipped: a
/// `--deny` that quietly did nothing would leave a build looking stricter than
/// it is. It is reported by the caller because a DiagnosticSink holds a policy,
/// so this header cannot see one.
std::vector<DiagnosticId> diagnosticIdsFor( std::span<std::string const> codes, std::vector<std::string>& unknown );

} // namespace nga::diag
