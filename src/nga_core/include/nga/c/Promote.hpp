#pragma once

#include "nga/c/Compile.hpp"
#include "nga/c/Ir.hpp"

#include <map>
#include <optional>
#include <set>
#include <span>
#include <string>

namespace nga::c
{

/// The objects `promoteGlobals` may hold, by name, with the type each is
/// defined in.
using Promotable = std::map<std::string, ir::Type, std::less<>>;

/// The scalar objects at file scope a loop may hold in a byte of its own Proc:
/// of a type of one or two bytes, defined once in the program's C, neither
/// `volatile` nor in a Pane nor a Slot's, and whose address no C of the
/// program takes — so that no pointer reaches it. Over the whole program,
/// since an address taken in one file reaches the object in another. See
/// docs/decisions/0286-a-loop-holds-a-global-in-a-byte-of-its-own.md.
[[nodiscard]] Promotable promotableGlobals( std::span<std::optional<ir::Unit> const> units );

/// The functions whose loops may be entered again and again, by name: one
/// called from inside a loop, one called by a function that is, and one whose
/// address the program takes, since a call through a pointer may stand
/// anywhere. Over the whole program, after `wrapCalls`, so that a body spliced
/// into its caller is counted where it now stands. A loop of any other
/// function, not inside another loop of it, is entered once for each time the
/// function is, which is taken to be seldom — see
/// docs/decisions/0303-fit-holds-an-object-where-the-cycles-pay-for-the-bytes.md.
[[nodiscard]] std::set<std::string> enteredOften( std::span<std::optional<ir::Unit> const> units );

/// A loop that calls nothing, and whose every way out leads only to blocks
/// entered from the loop alone, holds each of those objects it names in a byte
/// of its Proc — not one the function shadows with a parameter, a local, a
/// `static` local or a `const` of the same name: read into it where the loop is entered, the loop naming the
/// byte instead, and written back on every way out where the loop writes it.
/// The byte lies on the zero page and is a Temporary like any other, and the
/// loop's own passes — the invariant hoisted, the pointer walked beside a
/// counter — see it as the Proc's. Under `speed` wherever it pays. Under `size`
/// only where the loop's accesses, a byte shorter each, pay for the copies in
/// and out, so that the text grows nowhere. Under `fit` there too, and where
/// the text grows by a few bytes for a loop that saves many cycles: one whose
/// turns are known saving several times what the copies cost each time it is
/// entered, or, where they are not, one entered seldom, as `often` says.
void promoteGlobals( ir::Unit& unit, Promotable const& promotable, std::set<std::string> const& often, Intent intent );

} // namespace nga::c
