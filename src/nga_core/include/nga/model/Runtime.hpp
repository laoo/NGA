#pragma once

#include "nga/diag/SourceManager.hpp"
#include "nga/model/Project.hpp"

#include <string>
#include <string_view>

namespace nga::model
{

/// The Module name of the C runtime: dotted, as the tool's own Modules are,
/// so that no file of a Project is named so.
constexpr std::string_view C_RUNTIME_MODULE = "nga.runtime";

/// The text of the C runtime: a Proc per product, quotient and remainder
/// the compiler calls where no constant makes the operator shifts, adds and
/// masks, each held to the header of docs/spec/syntax.md#procedures — see
/// docs/decisions/0095-literals-and-the-runtime.md.
///
/// The Intent chooses among the shapes the text holds, through a Constant the
/// text declares and `.if` decides on: under `size` a wide multiply of
/// sixteen rounds stands where the one that stops has, and the divisions lose
/// the loop that a divisor of one byte takes. One text and one set of
/// conditionals, since two texts would be two copies of the same arithmetic
/// — see docs/decisions/0180-the-runtime-has-a-small-shape.md.
std::string cRuntimeSource( Intent intent = Intent::FIT );

/// Whether a Project holds a `.ngc` Module, which is given the runtime.
bool holdsC( diag::SourceManager const& sources, Project const& project );

/// Adds the runtime to a Project as a Module every Phase needs, and derives
/// Residency again, as the Transition routine is added. Prune drops every Proc
/// of it no call reaches.
void addCRuntime( Project& project, diag::SourceManager& sources );

} // namespace nga::model
