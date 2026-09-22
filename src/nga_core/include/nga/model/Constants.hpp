#pragma once

#include "nga/diag/SourceManager.hpp"
#include "nga/model/Module.hpp"
#include "nga/model/Project.hpp"

namespace nga::model
{

/// The Module whose Symbols are the Project's own Constants: one exported
/// Symbol of kind `Constant` per entry of a `constants` block, and no
/// Sections, so that a value a document declares is reached from every Module
/// the way an export is, through Merge, and no Step learns that a name can
/// come from anywhere but a Module. Built rather than assembled, from
/// `entry`, which is the Project's `constantModule` — see
/// docs/decisions/0178-a-configuration-is-a-document.md.
Module buildConstantModule( diag::SourceManager const& sources,
                            ProjectModule const& entry,
                            std::span<ProjectConstant const> constants );

} // namespace nga::model
