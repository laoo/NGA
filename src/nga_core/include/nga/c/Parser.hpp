#pragma once

#include "nga/c/SyntaxTree.hpp"
#include "nga/c/Token.hpp"
#include "nga/diag/DiagnosticSink.hpp"
#include "nga/diag/SourceManager.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

namespace nga::c
{

/// The grammar of docs/spec/c-subset.md, by recursive descent, to a
/// TranslationUnit.
///
/// Recognises what the text alone settles and nothing more: whether an
/// assignment's left side can be assigned, and whether a called name exists,
/// are the compiler's questions.
///
/// Recovery is C's usual one: report, skip to the `;` that ends the statement
/// or the declaration — or past the `}` of a block the skip entered — and
/// carry on, so that one run reports every error in the file. Each parse
/// function recovers from its own errors, so its caller carries on at once.
class Parser
{
public:
  Parser( diag::SourceManager const& sources, std::span<Token const> tokens, diag::DiagnosticSink& sink );

  TranslationUnit parseTranslationUnit();

private:
  [[nodiscard]] Token const& peek() const
  {
    return mTokens[mIndex];
  }

  [[nodiscard]] bool at( TokenKind wanted ) const
  {
    return peek().kind == wanted;
  }

  /// The token `ahead` places past the cursor, which is the end-of-file token
  /// past the end.
  [[nodiscard]] Token const& peekAhead( std::size_t ahead ) const
  {
    return mTokens[std::min( mIndex + ahead, mTokens.size() - 1 )];
  }

  [[nodiscard]] bool atKeyword( Keyword wanted ) const
  {
    return peek().keyword == wanted;
  }

  /// Whether the token is `const` or `volatile`.
  [[nodiscard]] static bool isQualifier( Token const& token )
  {
    return token.keyword == Keyword::CONST_QUALIFIER || token.keyword == Keyword::VOLATILE;
  }

  [[nodiscard]] bool atQualifier() const
  {
    return isQualifier( peek() );
  }

  /// `const` and `volatile` written in a row, in either order.
  struct Qualifiers
  {
    bool isConst = false;
    bool isVolatile = false;
  };

  /// The qualifiers at the cursor taken into `into`, each once: one written
  /// again is left, which is no name, as C would take it twice.
  void matchQualifiers( Qualifiers& into );

  Token advance();
  bool match( TokenKind wanted );

  /// Reports that `expected` was wanted where the current token stands, unless
  /// the lexer rejected that token, which has said all there is to say.
  void fail( std::string_view expected );

  /// Reports that `what` nest deeper than `limit` at `where`.
  void refuseDepth( Token const& where, std::string_view what, std::uint32_t limit );

  /// Skips past the `;` ending what failed, or past the `}` closing a block
  /// entered while skipping. A `}` the skip did not open ends a statement in
  /// its block and is left for the block; at the top level there is no block,
  /// so it is skipped too.
  void recover( bool atTopLevel );

  /// With the cursor on the type, and `storage` the `static` before it, if one
  /// was written.
  std::optional<FunctionDefinition> parseFunctionDefinition( std::optional<Token> storage,
                                                             std::optional<Token> inlined = std::nullopt );

  /// A type named in parentheses, as a cast or `sizeof` writes one: its
  /// keyword, whether it is a pointer to what that names, to `const` or
  /// `volatile`, and the
  /// tokens it takes.
  struct TypeName
  {
    Token keyword;
    bool pointer = false;
    bool pointeeConst = false;
    bool pointeeVolatile = false;
    std::size_t length = 0;
  };

  /// The type named in parentheses at the next token, where one is: reporting
  /// `int` or `char`, and taking nothing.
  std::optional<TypeName> parenthesisedType();

  ExpressionPtr parseSizeof();

  /// The lists of attributes at the next token, `[[...]]` after `[[...]]`, or
  /// none; nothing where one fails, which has been reported and skipped.
  std::optional<std::vector<Attribute>> parseAttributes();
  [[nodiscard]] bool atAttributes() const;
  std::optional<StructSpecifier> parseStructSpecifier();

  /// A value a list holds: an expression, or a list of its own in braces.
  ExpressionPtr parseListElement();
  std::optional<Declaration> parseDeclaration( std::optional<Token> storage );
  [[nodiscard]] bool functionAhead() const;

  /// With the cursor on `enum`.
  std::optional<EnumSpecifier> parseEnumSpecifier();

  /// With the cursor on the `{` of a `switch`, the labels and statements into
  /// `control`'s items.
  void parseSwitchBody( Statement& control );

  /// With the cursor on `{`.
  std::optional<Statement> parseCompoundStatement();
  std::optional<Statement> parseStatement();

  /// With the cursor on `if`, `while`, `do` or `for`.
  std::optional<Statement> parseControl();

  /// `( expression )`, reporting what is missing without recovering. Where
  /// `initialised` is given and a `;` stands before the `)`, what precedes it
  /// is that statement's initialiser — a declaration or an expression — and
  /// goes into it.
  ExpressionPtr parseCondition( Statement* initialised = nullptr );

  /// Whether a declaration of locals begins at the cursor.
  [[nodiscard]] bool declarationAhead() const;

  /// Whether the clause the cursor stands in holds a `;` before its `)`.
  [[nodiscard]] bool initialiserAhead() const;

  /// The statement a control statement governs, or a null statement standing
  /// for one that failed and has been reported, so that the control statement
  /// keeps its shape.
  Statement parseBody();

  /// Nothing when the expression was refused, and the refusal reported.
  ExpressionPtr parseAssignment();

  /// The binary operators binding at least as tightly as `minimum`, by
  /// precedence climbing: each descent is to a tighter level, so the descents
  /// are as many as the levels however long the expression is.
  ExpressionPtr parseBinary( int minimum );
  /// The binary operators, and the `? :` that may follow them.
  ExpressionPtr parseConditional();

  ExpressionPtr parseUnary();

  /// `left OP right` as a node of its own, the operator's kind given and the
  /// token the source wrote — `+=` for the `+` a compound assignment holds.
  ExpressionPtr operating( TokenKind kind, Token const& written, ExpressionPtr left, ExpressionPtr right );

  /// `++target` or `target--`: the assignment it stands for.
  ExpressionPtr parseStep( Token const& op, ExpressionPtr target, bool writtenAfter );
  ExpressionPtr parsePostfix();
  ExpressionPtr parsePrimary();
  ExpressionPtr parseStringLiteral();
  ExpressionPtr parseCharacterConstant();

  diag::SourceManager const* mSources;
  diag::DiagnosticSink* mSink;

  /// The tokens without their comments, ending in an end-of-file token.
  std::vector<Token> mTokens;
  std::size_t mIndex = 0;

  /// Blocks entered, parentheses open and assignments whose right side is
  /// being read: the descents the limits of SyntaxTree.hpp are counted on,
  /// refused before they are made, since making one is what spends the stack.
  std::uint32_t mBlocks = 0;
  std::uint32_t mParentheses = 0;
  std::uint32_t mAssignments = 0;
};

/// Lexes and parses one `.ngc` file.
TranslationUnit parse( diag::SourceManager const& sources, diag::FileId file, diag::DiagnosticSink& sink );

} // namespace nga::c
