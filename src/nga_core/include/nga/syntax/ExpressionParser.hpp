#pragma once

#include "nga/diag/DiagnosticSink.hpp"
#include "nga/syntax/Expression.hpp"
#include "nga/syntax/TokenCursor.hpp"

#include <cstdint>

namespace nga::syntax
{

/// The expression grammar of docs/spec/syntax.md, as a Pratt parser.
///
/// Deliberately knows nothing about `.asm`: no mnemonics, no addressing modes,
/// no directives. The Project file needs the same expressions
/// ([0003](docs/decisions/0003-project-file-syntax.md)), and this staying free
/// of the assembler is what makes that a grammar rather than a second parser.
///
/// It also never resolves a name and never folds a constant. A node's type
/// depends on what its names turn out to be, and that is not known until Merge.
class ExpressionParser
{
public:
  ExpressionParser( TokenCursor& cursor, diag::DiagnosticSink& sink );

  /// Parses one expression, stopping at the first token that cannot continue
  /// it — a comma, a closing parenthesis, a line ending. Always returns a node;
  /// on bad input the node is ERROR and the diagnostic has been raised.
  ExpressionPtr parse();

private:
  /// Binding powers, weakest first. The spacing leaves room to insert a level
  /// without renumbering, which is the only reason they are not 1..13.
  enum BindingPower : std::uint8_t
  {
    NONE = 0,
    /// The conditional value, loosest of all as in C, and right-associative:
    /// `a ? b : c ? d : e` chooses `c ? d : e` when `a` is false.
    SELECT = 5,
    LOGICAL_OR = 10,
    LOGICAL_AND = 20,
    BITWISE_OR = 30,
    BITWISE_XOR = 40,
    BITWISE_AND = 50,
    EQUALITY = 60,
    RELATIONAL = 70,
    /// Byte extraction is the one level C does not have. Below the comparisons
    /// so that `<foo == 0` compares the low byte, above the shifts so that
    /// `<foo+1` is the low byte of `foo+1`.
    BYTE_EXTRACTION = 80,
    SHIFT = 90,
    ADDITIVE = 100,
    MULTIPLICATIVE = 110,
    PREFIX = 120,
    ATTRIBUTE = 130,
  };

  ExpressionPtr parseAt( int minimumPower );
  /// `? a : b` after the condition, which has been read. Builds the two
  /// nodes the conditional is held as.
  ExpressionPtr parseSelect( ExpressionPtr condition );

  ExpressionPtr parsePrefix();
  ExpressionPtr parsePrimary();
  ExpressionPtr parseParenthesised();

  /// Reads an operand one level further in — under an operator, or inside a
  /// parenthesis when `parenthesis` says so — or refuses to, once that kind
  /// of level is MAX_EXPRESSION_DEPTH deep: the rest of the expression is
  /// then skipped rather than descended into, since the descent is itself
  /// what spends the stack the limit protects.
  ExpressionPtr descend( int minimumPower, bool parenthesis );

  /// The node as built, or an error in its place where operators now nest
  /// deeper than the limit — which is how a chain the loop builds without
  /// descending, `1+1+1…`, is held to it.
  ExpressionPtr bounded( ExpressionPtr node );

  /// Once per expression: every level above the one refused would say it
  /// again, and a parenthesis the skip could not reach unclosed on top.
  void reportTooDeep( diag::SourceSpan at );

  /// Consumes what is left of an expression, without recursion: tokens that
  /// can stand in one, parentheses balanced, stopping before a `)` or `:`
  /// that belongs to a level above. Returns what was consumed.
  diag::SourceSpan skipRest();

  /// Refuses to resolve C's two precedence traps rather than picking a winner,
  /// and says whether it did — a refusal produces no reading at all, so what
  /// is left is an error node and everything above it stays quiet.
  [[nodiscard]] bool rejectAmbiguousMix( Expression const& node ) const;

  void report( diag::Diagnostic value ) const;

  TokenCursor* mCursor;
  diag::DiagnosticSink* mSink;

  /// The levels open above the operand being read, of either kind.
  std::uint32_t mOperators = 0;
  std::uint32_t mParentheses = 0;

  /// Whether this expression has been refused as too deep, after which the
  /// parser says nothing more about it.
  bool mTooDeep = false;
};

} // namespace nga::syntax
