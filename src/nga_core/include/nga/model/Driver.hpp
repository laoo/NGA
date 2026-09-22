#pragma once

#include "nga/diag/DiagnosticSink.hpp"
#include "nga/diag/SourceManager.hpp"
#include "nga/model/Module.hpp"
#include "nga/model/Project.hpp"

#include <span>

namespace nga::model
{

/// The end of Assemble for the storage driver: finds the one Module that
/// declares it, holds it to the roles the routine calls and to `show` and
/// `showAt` for every Window the Target declares, to naming a `stream` that
/// shows the units storage is, and to being Resident; sets the Target's
/// stream ranges from it; and keeps it and every Module holding a decoder
/// outside them, since both run while the stream shows a Bank there. A
/// program that takes a `.transition` and declares no driver is refused
/// here. See docs/spec/transition.md.
void resolveDriver( diag::SourceManager const& sources,
                    Project& project,
                    std::span<Module> modules,
                    diag::DiagnosticSink& sink );

} // namespace nga::model
