#pragma once

#include "nga/diag/DiagnosticSink.hpp"
#include "nga/model/Build.hpp"
#include "nga/model/Merge.hpp"
#include "nga/model/Place.hpp"
#include "nga/model/Project.hpp"
#include "nga/model/Size.hpp"
#include "nga/model/Storage.hpp"

namespace nga::model
{

/// Re-reads a Layout and reports every Constraint it does not satisfy.
///
/// Not a Step: it decides nothing and changes nothing, and a run without it
/// produces the same bytes. It exists because Place is a constraint model
/// handed to a solver — a start variable per Claim, a no-overlap per Phase —
/// and this is a predicate: for every pair of Sections, do their ranges
/// overlap while their Residencies intersect. The two cannot share a defect,
/// because they share neither code nor shape, and that asymmetry is the whole
/// of what makes checking a solver's own answer worth anything. See
/// docs/decisions/0005-test-strategy.md.
///
/// What it holds the Layout to, which is every Constraint the model can state
/// today:
///
/// - no two Sections overlap while some Phase holds both — the co-visibility
///   rule of 0002, with the View half reading "always co-visible" until Views
///   exist, exactly as Place reads it — unless both are Temporaries the
///   Interference lets share, which is the rule's second index (0034)
/// - a pinned Section stands at the address its source declared
/// - a Proc stands immediately after the Proc whose `then` names it, in
///   every Phase both stand in
/// - a Movable Section stands at one address across every Residency a
///   Reference sees it from — the Freezes — and at every address it stands
///   at, the rules below hold
/// - an aligned Section starts at a multiple of its alignment
/// - a Section given a boundary does not cross a multiple of it
/// - a zero page Section ends below `$0100`
/// - a Section with a Payload lies wholly outside the Window
/// - a Section the solver placed lies wholly inside the pool its
///   PlacementClass names; a *pinned* one is exempt, because a pin below the
///   pool is a Constraint the author chose and 0018 says the solver honours it
/// - no Section reaches past the end of memory
/// - every Section has an address
///
/// Views are named by docs/spec/testing.md and are not here, because nothing
/// in the model declares them yet; Regions are read through the pools they
/// leave. Each is one predicate in this file on the day it is declared.
///
/// Call it on a Layout that Place did not refuse: after an error, Sections are
/// legitimately unplaced and every finding here would be that error again in
/// another voice.
void verifyLayout( Placed const& build, Storage const& storage, diag::DiagnosticSink& sink );

} // namespace nga::model
