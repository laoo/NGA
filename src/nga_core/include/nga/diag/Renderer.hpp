#pragma once

#include "nga/diag/DiagnosticSink.hpp"
#include "nga/diag/SourceManager.hpp"

#include <string>

namespace nga::diag
{

/// Human-readable rendering: a caret excerpt for located findings, a plain
/// heading for whole-program ones. Notes follow their parent.
std::string renderText( SourceManager const& sources, DiagnosticSink const& sink );

/// Machine-readable rendering, schema 1 of docs/spec/diagnostics.md.
///
/// `message` is present for convenience but is explicitly NOT stable; `id`,
/// `name` and `arguments` are what a consumer may rely on.
std::string renderJson( SourceManager const& sources, DiagnosticSink const& sink );

} // namespace nga::diag
