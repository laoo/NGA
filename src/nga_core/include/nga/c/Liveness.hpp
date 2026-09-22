#pragma once

#include "nga/c/Ir.hpp"

#include <set>
#include <string>
#include <vector>

namespace nga::c
{

/// The bytes of a Proc's own — its locals and its parameters — that are read
/// before they are written on some way out of each block: what is **live** as
/// the block is entered, by the block's index. A byte not live where a store
/// to it stands is a byte the store need not write. See
/// docs/decisions/0111-a-store-nothing-reads-is-not-written.md.
///
/// Only whole bytes of the Proc: a `static` local is a Section and outlives the
/// call, a local whose address is taken is a Section too, the result is read by
/// the caller, and a member's parameters are its type's. A `switch` reads and
/// writes the function's locals from Procs of its own, so it counts as reading
/// every one.
std::vector<std::set<std::string>> liveInOf( ir::Function const& function );

/// Drops every store to a byte of the Proc's own that nothing reads afterwards,
/// and the computation that fed it where nothing else does. First, a store
/// followed at once by a store of what it wrote — `x = x + x; return x;` — has
/// the second read what the first was given, so that the first has no reader
/// left.
void dropDeadStores( ir::Unit& unit );

} // namespace nga::c
