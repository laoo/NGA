#pragma once

#include "nga/model/Binding.hpp"
#include "nga/model/Charset.hpp"
#include "nga/model/Project.hpp"
#include "nga/model/Section.hpp"
#include "nga/syntax/Expression.hpp"

#include <cstdint>
#include <optional>
#include <string_view>
#include <unordered_map>
#include <variant>
#include <vector>

namespace nga::model
{

/// What defined a Symbol, and therefore what may be done with it. Separate from
/// the **type** of its expression, which says what it evaluates to.
enum class SymbolKind : std::uint8_t
{
  LABEL,
  CONSTANT,
  CHARSET,
  SLOT,
  REGION,
  WINDOW,
  PANE,
  MACRO,
};

std::string_view nameOf( SymbolKind kind );

/// A Label's value: a position, and never an offset, because a definition can
/// only stand at a Chunk boundary.
///
/// `inner` is set for one thing only: the position a local label of a macro
/// body names once the body is expanded, which lies inside the use's Chunk,
/// at that index among its inner Chunks. No Symbol has one — nothing outside
/// an expansion can name a position inside it — so a Label never carries it.
struct LabelPosition
{
  SectionIndex section;
  ChunkIndex chunk;
  std::optional<std::uint32_t> inner;

  friend bool operator==( LabelPosition, LabelPosition ) = default;
};

/// The Section a local label of a macro body names while the body is still
/// a template: no Section of the Module, since the template is placed by
/// nothing. Expand translates it into the use's Section, and nothing else
/// ever sees it.
constexpr SectionIndex MACRO_BODY_SECTION{ 0xFFFFFFFFU };

/// What `.slot` declared: the contract every Implementation and every use is
/// held to. The Cell is the Section the tool builds for it at the end of
/// Assemble, once every Slot of the program is known — see
/// docs/decisions/0031-slots.md.
struct SlotDeclaration
{
  Binding binding = Binding::POINTER;
  PlacementClass placement = PlacementClass::ABSOLUTE;
  std::optional<SectionRef> cell;
};

/// What a named Region's Symbol stands for: the Region, and its start
/// address — an `Integer`, since a Region's address is a value the variant
/// wrote out, which is how hardware registers have always been reached. The
/// address is here so that evaluating the name needs no Target.
struct RegionValue
{
  RegionIndex region;
  std::uint32_t address = 0;
};

/// A named value of a definite kind.
///
/// A Constant holds an **expression**, not a number: `operand = label + 1` is a
/// Constant of `Address` type, so nothing here can be reduced to an integer
/// while a Module is assembled.
using SymbolValue = std::variant<LabelPosition,
                                 syntax::ExpressionPtr,
                                 CharsetIndex,
                                 SlotDeclaration,
                                 RegionValue,
                                 WindowIndex,
                                 PaneIndex,
                                 MacroIndex>;

struct Symbol
{
  std::string_view name;
  SymbolKind kind = SymbolKind::CONSTANT;
  diag::SourceSpan definition;
  bool exported = false;

  SymbolValue value;
};

/// The Symbols one Module defines.
///
/// Definition order is kept, and it is the order everything iterates in:
/// diagnostics are part of the output contract, and walking a hash table to
/// produce them is the quietest way there is to make a build non-reproducible.
class SymbolTable
{
public:
  [[nodiscard]] Symbol const* find( std::string_view name ) const;

  /// Null when the Symbol was added; the existing definition when the name was
  /// already taken, so the caller can report both positions. The pointer is
  /// good until the next call to add.
  Symbol const* add( Symbol symbol );

  /// Makes a Symbol visible outside its Module. False when this Module defines
  /// nothing of that name, which is the whole of what `.export` can get wrong.
  bool markExported( std::string_view name );

  [[nodiscard]] std::vector<Symbol> const& symbols() const
  {
    return mSymbols;
  }

  /// For the end of Assemble alone, which gives a Slot its Cell once every
  /// Slot of the program is known. Nothing else changes a Symbol.
  [[nodiscard]] Symbol& at( std::uint32_t index )
  {
    return mSymbols[index];
  }

private:
  std::vector<Symbol> mSymbols;
  std::unordered_map<std::string_view, std::uint32_t> mIndex;
};

} // namespace nga::model
