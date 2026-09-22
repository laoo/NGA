#pragma once

#include "nga/diag/SourceLocation.hpp"
#include "nga/diag/SourceManager.hpp"
#include "nga/syntax/Token.hpp"

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

namespace nga::syntax
{

/// What an expression evaluates to, per docs/spec/syntax.md.
///
/// Not stored on a node: the type of anything naming a Symbol is unknown until
/// the Symbol is, which is after Merge for a name from another Module. The enum
/// lives here because the tree is what a type check runs over.
enum class ExpressionType : std::uint8_t
{
  UNKNOWN,
  INTEGER,
  ADDRESS,
  STRING,
  PANE,
};

std::string_view nameOf( ExpressionType type );

enum class ExpressionKind : std::uint8_t
{
  /// Stands in for input that has already been diagnosed. Everything above it
  /// stays quiet, which is what keeps one bad operand from producing a column
  /// of errors.
  ERROR,

  NUMBER,
  CHARACTER,
  STRING,

  /// A number the tool computed rather than the author wrote: an argument of
  /// a macro use that had a declared value, folded by Expand before it is
  /// substituted, or the sum an argument `U + c` carries, and held on the
  /// node since no text spells it. See docs/decisions/0049-recursive-macros.md
  /// and 0070-expression-depth.md.
  VALUE,

  NAME,       ///< an identifier
  LOCAL_NAME, ///< `@name` or `@`, carrying a Direction on its token

  /// `name...` as an item of a list: the elements of the pack named, spread
  /// in its place. Made by the parser in a list alone, and replaced by Expand
  /// before any Step reads the list — see docs/decisions/0050-packs.md.
  SPREAD,
  ATTRIBUTE, ///< `expression . name`, which is also the qualified-name form

  UNARY,
  BINARY,
};

enum class UnaryOperator : std::uint8_t
{
  NEGATE,     ///< `-`
  COMPLEMENT, ///< `~`
  NOT,        ///< `!`
  LOW_BYTE,   ///< `<`
  HIGH_BYTE,  ///< `>`
};

enum class BinaryOperator : std::uint8_t
{
  ADD,
  SUBTRACT,
  MULTIPLY,
  DIVIDE,
  SHIFT_LEFT,
  SHIFT_RIGHT,
  BITWISE_AND,
  BITWISE_OR,
  BITWISE_XOR,
  EQUAL,
  NOT_EQUAL,
  LESS,
  LESS_EQUAL,
  GREATER,
  GREATER_EQUAL,
  LOGICAL_AND,
  LOGICAL_OR,

  /// `cond ? a : b`, held as two nodes: SELECT over the condition and an ARM,
  /// which pairs the two answers. Two binary nodes rather than one node of
  /// three children so that every walk over a tree stays a walk over `left`
  /// and `right` — see docs/decisions/0047-a-conditional-value.md.
  SELECT,
  ARM,
};

/// How the operator is written, for diagnostics that have to name it.
std::string_view spellingOf( UnaryOperator op );
std::string_view spellingOf( BinaryOperator op );

/// Which family an operator belongs to. Used by the rule that refuses to
/// resolve C's two notorious precedence traps.
bool isAdditive( BinaryOperator op );
bool isShift( BinaryOperator op );
bool isBitwise( BinaryOperator op );
bool isComparison( BinaryOperator op );

struct Expression;
using ExpressionPtr = std::unique_ptr<Expression>;

/// One node of an expression tree.
///
/// The tree exists because a Label's value is unknown until Place, so an
/// expression has to survive unevaluated all the way to Patch — see
/// docs/decisions/0009-assemble-builds-model-objects.md. Nothing here folds,
/// and nothing here resolves a name; the one folded node, VALUE, is made by
/// Expand from an argument's value, or from the sum an argument carries.
struct Expression
{
  ExpressionKind kind = ExpressionKind::ERROR;

  /// True when this node came directly from a parenthesised group. The author
  /// said which reading they meant, so the rule about C's precedence traps has
  /// nothing left to refuse.
  bool parenthesised = false;

  /// True on a NAME written with a leading dot, `.count`. The dot separates a
  /// scope from a name, and with nothing on its left the scope is the top
  /// level: the name is read there whatever Namespace or Proc it stands in.
  /// See docs/decisions/0046-a-proc-is-a-scope.md.
  bool fromRoot = false;

  UnaryOperator unaryOperator{};
  BinaryOperator binaryOperator{};

  /// The token that names or spells this node: the literal for NUMBER,
  /// CHARACTER and STRING, the identifier for NAME and LOCAL_NAME, the
  /// attribute name for ATTRIBUTE, the operator for UNARY and BINARY. Text is
  /// read through the SourceManager and never copied.
  Token token;

  /// The whole of what this node covers, which is what a diagnostic underlines.
  diag::SourceSpan span;

  /// What a VALUE stands for. Meaningless on every other kind.
  std::int64_t value = 0;

  /// UNARY uses `left` alone; ATTRIBUTE uses it for the receiver.
  ExpressionPtr left;
  ExpressionPtr right;
};

ExpressionPtr makeExpression( ExpressionKind kind, Token token, diag::SourceSpan span );

/// Whether anything in this tree has already been reported on. A statement
/// holding one has said all it has to say, so the grammar recovers from it in
/// silence rather than stacking a second finding on the same line.
bool containsError( Expression const& node );

/// How deep operators may nest in one expression, and how deep parentheses
/// may. Every walk over a tree — the type check, evaluation, the search for
/// References — recurses once per level, and what that spends is a thread's
/// stack, which is one megabyte on Windows while a level of the type check
/// is kilobytes in a debug build. C asks a compiler to take 63 nested
/// parentheses; 64 is room for any expression a program writes. The parser
/// refuses more (`NGA0104`), and so does Expand, where an argument can grow
/// with a recursion (`NGA2333`). See docs/decisions/0070-expression-depth.md.
constexpr std::uint32_t MAX_EXPRESSION_DEPTH = 64;

/// How many operators deep the tree goes, none for a leaf; a conditional is
/// two, since it is held as two nodes. Recursive, and safe to be: a tree is
/// measured before it is handed on, and nothing builds one deeper than twice
/// the limit.
std::uint32_t depthOf( Expression const& node );

/// The span covering both ends, for a node built from two others.
diag::SourceSpan spanning( diag::SourceSpan first, diag::SourceSpan second );

/// The dotted name a tree spells when it is nothing but names joined by
/// dots — `one.two.gfx` — and nothing otherwise. What a qualified name looks
/// like before anything says whether `one` is a Namespace or a Section.
std::optional<std::string> dottedNameOf( diag::SourceManager const& sources, Expression const& node );

/// The leftmost node of a dotted chain: the name whose scope the whole
/// chain is read in.
Expression const& leftmostOf( Expression const& node );

} // namespace nga::syntax
