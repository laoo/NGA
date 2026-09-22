#pragma once

#include "nga/diag/DiagnosticSink.hpp"
#include "nga/diag/SourceManager.hpp"
#include "nga/model/Module.hpp"

#include <span>

namespace nga::model
{

/// Applies every `.off` of every Module to what the run reported: a warning
/// standing on the line a `.off` named, with the identifier it named, is
/// dropped. Run once, after the last Step, because the warning may come from
/// any of them. A `.off` that silenced nothing is a warning of its own, and
/// only in a run without errors — after an error a later Step never ran,
/// and what it would have reported is not known. See
/// docs/decisions/0039-off.md.
void applySuppressions( diag::SourceManager const& sources,
                        std::span<Module const> modules,
                        diag::DiagnosticSink& sink );

} // namespace nga::model
