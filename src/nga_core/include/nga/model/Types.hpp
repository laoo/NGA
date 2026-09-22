#pragma once

#include "nga/model/PlacementClass.hpp"
#include "nga/model/Project.hpp"
#include "nga/model/Section.hpp"
#include "nga/syntax/Expression.hpp"

#include <cstdint>

namespace nga::model
{

/// Which coordinate system an address is in.
///
/// Runtime and storage are separate systems assigned by separate Steps, so
/// mixing them is an error rather than a number. The tag is also what makes
/// leaving the storage model unsettled safe rather than merely late.
enum class Space : std::uint8_t
{
  RUNTIME,
  STORAGE,
};

std::string_view nameOf( Space space );

/// What an expression evaluates to.
///
/// An Address carries three things beyond being an address, and each of them
/// exists to make one rule expressible rather than to describe an address: the
/// space separates runtime from storage, the Section identity is what makes
/// subtraction decidable, and the PlacementClass is what decides the width of
/// an instruction that uses it. See
/// docs/decisions/0010-expression-types.md.
struct Type
{
  syntax::ExpressionType kind = syntax::ExpressionType::UNKNOWN;

  /// Meaningful for ADDRESS.
  Space space = Space::RUNTIME;

  /// Meaningful for ADDRESS and SECTION.
  SectionRef section;

  /// Meaningful for ADDRESS.
  PlacementClass placement = PlacementClass::ABSOLUTE;

  /// Meaningful for PANE: which Pane, and how many members it has — one for
  /// a Pane, more for a family, of which `member` is the one named so far.
  PaneIndex pane{};
  std::uint32_t paneCount = 1;
  std::uint32_t member = 0;

  /// False once something has been reported about this expression, which is
  /// how one bad operand avoids becoming a column of findings.
  [[nodiscard]] bool isKnown() const
  {
    return kind != syntax::ExpressionType::UNKNOWN;
  }

  [[nodiscard]] bool is( syntax::ExpressionType wanted ) const
  {
    return kind == wanted;
  }

  static Type unknown()
  {
    return Type{};
  }

  static Type integer()
  {
    return Type{ .kind = syntax::ExpressionType::INTEGER, .section = {} };
  }

  static Type string()
  {
    return Type{ .kind = syntax::ExpressionType::STRING, .section = {} };
  }

  static Type address( SectionRef where, PlacementClass placement, Space space = Space::RUNTIME )
  {
    return Type{ .kind = syntax::ExpressionType::ADDRESS, .space = space, .section = where, .placement = placement };
  }

  static Type paneOf( PaneIndex which, std::uint32_t count )
  {
    return Type{ .kind = syntax::ExpressionType::PANE, .section = {}, .pane = which, .paneCount = count, .member = 0 };
  }
};

} // namespace nga::model
