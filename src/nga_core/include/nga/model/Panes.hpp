#pragma once

#include "nga/diag/DiagnosticSink.hpp"
#include "nga/diag/SourceManager.hpp"
#include "nga/model/Module.hpp"
#include "nga/model/Project.hpp"

#include <span>

namespace nga::model
{

/// The end of Assemble for Panes: binds every Section that wrote `in PANE`
/// to the Pane the Project declares under that name, and holds it to what a
/// Pane's Section can be — `absolute`, since a Window covers no zero page,
/// and not Movable, since a Pane's Section stands in one Bank at one
/// address for the whole run. See docs/decisions/0054-panes.md.
void resolvePanes( diag::SourceManager const& sources,
                   Project const& project,
                   std::span<Module> modules,
                   diag::DiagnosticSink& sink );

} // namespace nga::model
