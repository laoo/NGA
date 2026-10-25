#pragma once

#include "nga/c/Ir.hpp"

#include <cstdint>
#include <map>
#include <optional>
#include <set>
#include <span>
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

/// Whether a call is one of the runtime's arithmetic, `__mul8`, `__mul16`,
/// `__mul8to16` and the divisions and remainders of either sign and width:
/// what it reads is its two arguments and what it writes is its own bytes, so
/// it changes no object of the program and answers the same for the same
/// operands — see
/// docs/decisions/0304-a-product-a-loop-computes-the-same-is-computed-before-it.md.
[[nodiscard]] bool isArithmetic( ir::Call const& call );

/// Every name a constant of the unit gives, which is every address it takes:
/// of an operand, a branch's condition, an argument, and what an object or a
/// named constant is given.
void addressesIn( ir::Unit const& unit, std::set<std::string>& taken );

/// What a function may write when it is called: the objects its stores name,
/// not its own bytes, and those of every function it calls; whether it, or one
/// of those, writes through a pointer; and whether it calls what nothing here
/// summarises — a Proc of the assembler, a call through a pointer, the
/// runtime's `__fill` — after which anything may have changed.
struct Writes
{
  std::set<std::string> objects;
  bool throughPointer = false;
  bool anything = false;
};

/// What every function of the program writes, by name, and which objects at
/// file scope a store through a pointer may reach: the ones whose address the
/// program's C takes, and no other — as
/// docs/decisions/0286-a-loop-holds-a-global-in-a-byte-of-its-own.md has it.
/// Over the whole program, after calls are wrapped — see
/// docs/decisions/0304-a-product-a-loop-computes-the-same-is-computed-before-it.md.
struct Effects
{
  std::map<std::string, Writes, std::less<>> functions;
  std::set<std::string> addressed;
};

[[nodiscard]] Effects effectsOf( std::span<std::optional<ir::Unit> const> units );

/// What the loop writes: every object a store of it names whole or in part,
/// and whether it writes through a pointer or calls anything but the runtime's
/// arithmetic, after which any object that is not the Proc's own may have
/// changed. Given what the program's functions write, a call of one adds what
/// it writes instead, and a store through a pointer reaches only the objects
/// whose address is taken, which `addressed` then names.
struct Written
{
  std::set<std::string> objects;
  bool anything = false;
  bool throughPointer = false;
  std::set<std::string> const* addressed = nullptr;
};

[[nodiscard]] Written writtenIn( ir::Function const& function, Loop const& loop, Effects const* effects = nullptr );

/// The bytes of the Proc's own, which nothing reaches but the Proc's own
/// stores: what stays invariant across a call or a store through a pointer.
[[nodiscard]] std::set<std::string> ownBytesOf( ir::Function const& function );

/// Whether an operand reads the same at every turn of the loop: a constant, or
/// an object the loop does not write. A value is its own block's and is never
/// taken for invariant here.
[[nodiscard]] bool invariant( ir::Operand const& operand, Written const& written, std::set<std::string> const& own );

} // namespace nga::c
