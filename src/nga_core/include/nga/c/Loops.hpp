#pragma once

#include "nga/c/Ir.hpp"

#include <cstdint>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace nga::c
{

/// A loop as the lowering lays one out: its header, the last block of its
/// body — the one that jumps back — and the block it is entered from, which
/// stands just before the header and goes nowhere else. What the passes over
/// loops share — see docs/decisions/0125-what-a-loop-computes-the-same-every-turn.md.
struct Loop
{
  std::uint32_t header = 0;
  std::uint32_t last = 0;
  std::uint32_t entry = 0;
};

/// Every loop of the function, outermost first: a block that goes back to one
/// at or before it closes a loop, and the loop is every block from that header
/// to it. Kept only where the header is entered from outside by the block
/// before it alone, which is where the lowering puts a loop's entry and where
/// what a pass computes before the loop can stand.
[[nodiscard]] std::vector<Loop> loopsOf( ir::Function const& function );

/// The one block whose terminator leads to this one, or nothing where none
/// does or more than one does.
[[nodiscard]] std::optional<std::uint32_t> onlyPredecessorOf( ir::Function const& function, std::uint32_t index );

/// What the loop writes: every object a store of it names whole or in part,
/// and whether it writes through a pointer or calls, after which any object
/// that is not the Proc's own may have changed.
struct Written
{
  std::set<std::string> objects;
  bool anything = false;
};

[[nodiscard]] Written writtenIn( ir::Function const& function, Loop const& loop );

/// The bytes of the Proc's own, which nothing reaches but the Proc's own
/// stores: what stays invariant across a call or a store through a pointer.
[[nodiscard]] std::set<std::string> ownBytesOf( ir::Function const& function );

/// Whether an operand reads the same at every turn of the loop: a constant, or
/// an object the loop does not write. A value is its own block's and is never
/// taken for invariant here.
[[nodiscard]] bool invariant( ir::Operand const& operand, Written const& written, std::set<std::string> const& own );

/// The object a name stands for, without the offset a byte of it carries:
/// `__0q+1` is `__0q`.
[[nodiscard]] std::string baseOf( std::string const& name );

} // namespace nga::c
