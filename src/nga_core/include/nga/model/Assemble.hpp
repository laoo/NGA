#pragma once

#include "nga/diag/DiagnosticSink.hpp"
#include "nga/diag/SourceManager.hpp"
#include "nga/model/Module.hpp"
#include "nga/model/Project.hpp"

#include <string>
#include <vector>

namespace nga::model
{

/// The Assemble Step for one Module: lex, parse, and build model objects.
///
/// Nothing here is shared with another Module, which is what lets this run on
/// as many threads as there are files with no locking and no ordering.
Module assembleModule( diag::SourceManager const& sources,
                       diag::FileId file,
                       std::string name,
                       Residency residency,
                       diag::DiagnosticSink& sink );

/// The Assemble Step for a whole Project.
///
/// Each Module reports into a sink of its own, and the sinks are merged in
/// Project order — so the diagnostic sequence is a property of the Project and
/// not of the order the work happened to run in. That is the contract this has
/// to satisfy before there is any concurrency to break it.
///
/// The Project is written to once: a program that declares a Slot gains the
/// Module holding its Cells, and one with Transitions the dispatcher over its
/// decoders, which only the end of Assemble can build — the second from
/// source, which is why the SourceManager is written to as well.
std::vector<Module> assembleProject( diag::SourceManager& sources, Project& project, diag::DiagnosticSink& sink );

} // namespace nga::model
