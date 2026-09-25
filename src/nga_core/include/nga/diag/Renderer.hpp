#pragma once

#include "nga/diag/DiagnosticSink.hpp"
#include "nga/diag/SourceManager.hpp"

#include <string>
#include <string_view>

namespace nga::diag
{

/// Human-readable rendering: a caret excerpt for located findings, a plain
/// heading for whole-program ones. Notes follow their parent.
std::string renderText( SourceManager const& sources, DiagnosticSink const& sink );

/// The findings as two JSON fields, `diagnostics` and `summary`, indented by
/// `indent` and without a trailing newline.
///
/// Fields rather than a document, because the findings are a set of the facts
/// of a build and travel inside that one -- see docs/spec/facts.md. `message`
/// is present for convenience but is explicitly NOT stable; `id`, `name` and
/// `arguments` are what a consumer may rely on.
void appendJsonFindings( std::string& out,
                         SourceManager const& sources,
                         DiagnosticSink const& sink,
                         std::string_view indent );

} // namespace nga::diag
