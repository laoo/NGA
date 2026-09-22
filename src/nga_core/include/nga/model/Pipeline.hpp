#pragma once

#include "nga/diag/DiagnosticSink.hpp"
#include "nga/diag/SourceManager.hpp"
#include "nga/model/Build.hpp"
#include "nga/model/Project.hpp"

#include <cstdint>
#include <functional>
#include <optional>
#include <vector>

namespace nga::model
{

/// What a build is asked to do beyond producing the Container.
struct BuildOptions
{
  /// Re-read the Layout and report a Constraint it does not satisfy — see
  /// docs/spec/testing.md.
  bool verifyLayout = false;

  /// Where no Layout exists, say which Sections cannot be kept apart.
  bool explain = false;
};

/// The Container a build wrote, as the Project's `container` chose it.
struct Emitted
{
  std::vector<std::uint8_t> bytes;

  /// Where a raw image starts, and nothing for a `.xex`, which says so itself.
  std::optional<std::uint32_t> origin;
};

/// Every Step, from Assemble to Emit, over one Project, and then what the
/// source asked to be silenced.
///
/// An error stops the build at the gate after the Step that raised it, and
/// the gates are here, once, so that the tool and the suite stop in the same
/// place: a case is held to the findings the tool shows, and a marker naming a
/// finding of a Step the gate kept from running is unmet — see
/// docs/spec/diagnostics.md and docs/spec/testing.md.
///
/// `built` is called only when every Step ran without an error, with the
/// finished build and the Container. The build lives no longer than the call,
/// so whatever reads it — the memory map, the suite's machine — reads it there.
void build( diag::SourceManager& sources,
            Project& project,
            BuildOptions const& options,
            diag::DiagnosticSink& sink,
            std::function<void( Patched const&, Emitted const& )> const& built );

} // namespace nga::model
