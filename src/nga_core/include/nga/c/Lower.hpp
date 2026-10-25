#pragma once

#include "nga/c/Front.hpp"
#include "nga/c/Ir.hpp"

#include <cstdint>
#include <set>

namespace nga::c::detail
{

/// One file that passed the check, in the compiler's own form: `sink` takes
/// the one finding only its blocks can decide, a function that returns
/// something and whose end is reached; `addressed` holds where each local
/// whose address is taken is declared.
ir::Unit lowerUnit( diag::SourceManager const& sources,
                    Names const& names,
                    Unit const& unit,
                    diag::DiagnosticSink& sink,
                    std::set<std::uint32_t> const& addressed,
                    Membership const& members );

} // namespace nga::c::detail
