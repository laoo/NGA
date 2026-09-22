#pragma once

#include "nga/c/Ir.hpp"

#include <cstdint>
#include <vector>

namespace nga::c
{

/// What a value can hold: every value it may take lies between `min` and
/// `max`, both included. Nothing narrows a range on a guess — where a value's
/// own operation says nothing, its range is the whole of its type — so a
/// reader may treat the bounds as a promise. See
/// docs/decisions/0121-what-a-value-can-hold.md.
struct Range
{
  std::int64_t min = 0;
  std::int64_t max = 0;

  [[nodiscard]] bool holds( std::int64_t value ) const
  {
    return value >= min && value <= max;
  }

  /// Whether every value this allows the other allows too.
  [[nodiscard]] bool within( Range other ) const
  {
    return min >= other.min && max <= other.max;
  }

  /// Whether it is one unsigned byte, which is what a reader asking about a
  /// high byte is asking.
  [[nodiscard]] bool isByte() const
  {
    return min >= 0 && max <= 0xFF;
  }
};

/// The range a type's own bits allow, which is what is known of a value the
/// analysis can say nothing else about.
[[nodiscard]] Range rangeOfType( ir::Type type );

/// The range of an operation over operands of these ranges, computed in
/// `type`: the exact range where it fits the type, and the type's own where
/// the operation may wrap out of it, since a wrapped result is no interval.
[[nodiscard]] Range rangeOfBinary( ir::BinaryOperator op, ir::Type type, Range left, Range right );
[[nodiscard]] Range rangeOfUnary( ir::UnaryOperator op, ir::Type type, Range operand );
[[nodiscard]] Range rangeOfConvert( ir::Type to, Range operand );

/// The range of every value of one function, by the value's index.
///
/// A sweep forward over the blocks in their order: the IR has no `phi`, a
/// value is defined by one instruction from operands defined before it, and
/// what merges where paths meet is a byte of the Proc and not a value. A byte
/// of the Proc's own — which nothing but its own stores reaches — is what its
/// last store left, narrowed past a branch on it, and where ways meet what
/// all of them agree on, where every way comes from an earlier block. A
/// loop's header takes as given that its bytes hold no less than on the way
/// in; where a store of the loop may go below, the header takes nothing of
/// that byte and the sweep runs again, which only ever takes back and so ends.
/// Any other byte is read as its type allows. See
/// docs/decisions/0134-a-number-added-to-an-index-goes-into-the-address.md.
[[nodiscard]] std::vector<Range> rangesOf( ir::Function const& function );

/// The range an operand reads as: a constant is itself, an object and a
/// constant the text names by its name are their type's, and a value is what
/// the table says.
[[nodiscard]] Range
rangeOf( ir::Operand const& operand, ir::Function const& function, std::vector<Range> const& ranges );

} // namespace nga::c
