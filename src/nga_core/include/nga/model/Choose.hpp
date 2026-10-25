#pragma once

#include "nga/diag/DiagnosticSink.hpp"
#include "nga/diag/SourceManager.hpp"
#include "nga/model/Build.hpp"
#include "nga/model/Patch.hpp"
#include "nga/model/Project.hpp"
#include "nga/model/Storage.hpp"

#include <span>
#include <vector>

namespace nga::model
{

/// The program as each Choice of a `choose` block but the first would build
/// it: the Project read with that Choice's registers and constants in place of
/// the first's, and nothing else of it changed. Empty where there is no
/// `choose` block. Taken before Assemble, which fills the Project as it runs —
/// see docs/decisions/0302-a-document-may-be-chosen-while-a-xex-loads.md.
[[nodiscard]] std::vector<Project> laterChoicesOf( diag::SourceManager const& sources, Project const& project );

/// The bytes the Choices differ in. Each later Choice is built from Assemble
/// through Size and patched over the first's Layout, the load's own code
/// included, and every byte of a Section that comes out otherwise is a site.
/// A Section whose size differs, or that one build reaches and the other does
/// not, is `NGA6245`, and a site in the load's own code `NGA6246`. What a later
/// build reports as an error is reported; its warnings are the first build's
/// again, and are not.
[[nodiscard]] std::vector<Storage::ChoiceSite> findChoiceSites( diag::SourceManager& sources,
                                                                std::span<Project const> later,
                                                                Placed const& build,
                                                                Storage const& storage,
                                                                Bytes const& bytes,
                                                                diag::DiagnosticSink& sink );

} // namespace nga::model
