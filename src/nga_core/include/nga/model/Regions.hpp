#pragma once

#include "nga/diag/SourceManager.hpp"
#include "nga/model/Module.hpp"
#include "nga/model/Project.hpp"

namespace nga::model
{

/// The Module whose Symbols are the Target's named Regions: one exported
/// Symbol of kind `Region` per name, and no Sections, so that a register is
/// reached from every Module the way an export is, through Merge, and no
/// Step learns that names come from anywhere but a Module. Built rather than
/// assembled, from `entry`, which is the Project's `regionModule`.
Module buildRegionModule( diag::SourceManager const& sources, ProjectModule const& entry, Target const& target );

} // namespace nga::model
