#pragma once

#include "nga/diag/SourceLocation.hpp"
#include "nga/syntax/Expression.hpp"
#include "nga/syntax/Token.hpp"

#include <cstdint>
#include <string>
#include <string_view>

namespace nga::syntax
{

// The words a statement says of itself, as the grammar reports them to its
// Builder. They are the model's vocabulary — the glossary names each — but
// they belong here, as OperandShape does, because they are what was written:
// the model reads them and the grammar never reads the model.

/// Whether an address lives in the zero page or anywhere else.
///
/// Declared, never inferred: instruction size depends on it and addresses
/// depend on instruction sizes, so reading it back from the address the solver
/// chose would close that loop. A Section declares it and its Labels inherit it.
enum class PlacementClass : std::uint8_t
{
  ZEROPAGE,
  ABSOLUTE,
};

std::string_view nameOf( PlacementClass placement );

/// How a Reference reaches a Slot's live definition: through the Cell the
/// Transition routine rewrites on every edge. See the glossary. `bank` waits
/// for Views, and `address` is not a Cell at all — it is what a Section that
/// never moves has, and a Slot is for what does.
enum class Binding : std::uint8_t
{
  /// Two bytes holding the definition's address: `lda (slot),y`, `jmp (slot)`.
  POINTER,

  /// `jmp` and two bytes: `jsr slot` calls whatever is live.
  VECTOR,
};

std::string_view nameOf( Binding binding );

/// What a `.with` wrote. See docs/decisions/0055-with.md,
/// 0068-with-shows-again-what-was-shown.md and 0229-no-window-has-a-base.md.
enum class WithForm : std::uint8_t
{
  NONE,

  /// `.with PANE`: the state the solver gave the Pane.
  SHOW,

  /// `.with FAMILY, x` or `.with WINDOW, x`: the state whose index is in X.
  AT,

  /// `.with FAMILY, xy` or `.with WINDOW, xy`: the state whose index's low
  /// byte is in X and high byte in Y, for a Window whose driver takes it so —
  /// see docs/decisions/0228-a-driver-names-the-cells-its-stream-stands-in.md.
  AT_XY,

  /// `.with WINDOW = STATE`: a named state of the Window.
  STATE,
};

/// Who follows an address a Chunk takes — see model::Chunk::taking.
enum class Taking : std::uint8_t
{
  ESCAPE,
  OWN,
  ROOT,
};

/// What a `.declare` says the `.ztemp` below it is to the Proc holding it:
/// one of its arguments, in the order the declarations stand in, or its
/// result — see docs/decisions/0081-a-procs-signature-is-declared.md.
enum class Declaring : std::uint8_t
{
  ARGUMENT,
  RESULT,
};

/// The type a declared byte of a Proc holds, where one was written. The
/// types of the C subset, and `u8[N]` for a run of bytes. Optional, because
/// the shape of the reservation answers where nothing was written, and held
/// to that shape where both are present.
struct DeclaredType
{
  enum class Kind : std::uint8_t
  {
    U8,
    I8,
    U16,
    I16,
    BOOL,
    BYTES,
  };

  Kind kind = Kind::U8;

  /// How many elements `u8[N]` has; one for every other kind.
  std::uint32_t count = 1;
  diag::SourceSpan span{};

  friend bool operator==( DeclaredType const& left, DeclaredType const& right )
  {
    return left.kind == right.kind && left.count == right.count;
  }

  /// How many bytes holding the type takes, which is what the reservation
  /// under the declaration is held to.
  [[nodiscard]] std::uint32_t bytes() const
  {
    switch ( kind )
    {
    case Kind::U16:
    case Kind::I16:
      return 2;
    case Kind::BYTES:
      return count;
    case Kind::U8:
    case Kind::I8:
    case Kind::BOOL:
      break;
    }
    return 1;
  }
};

/// How a declared type is written, which is how a finding names it.
[[nodiscard]] std::string spellingOf( DeclaredType const& type );

/// One entry of a `.charset`: a run of code points onto consecutive bytes.
///
/// The characters are kept as the literal that was written rather than as a
/// decoded list, because decoding is the resolving Step's business and a Module
/// is assembled before any of that can be answered.
struct CharsetEntry
{
  Token characters;    ///< the string literal, in the order written
  ExpressionPtr start; ///< the byte its first character maps to
};

/// What a derived `.charset` derives from.
struct CharsetBase
{
  Token name;

  /// Null when the derivation named no transform. There is exactly one
  /// transform and it is `^` — see docs/decisions/0013-charset-declaration.md.
  ExpressionPtr mask;
};

} // namespace nga::syntax
