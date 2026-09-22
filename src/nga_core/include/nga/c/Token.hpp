#pragma once

#include "nga/diag/SourceLocation.hpp"

#include <cstdint>
#include <string_view>

namespace nga::c
{

/// The lexical categories of docs/spec/c-subset.md.
///
/// Every punctuator of C23 is here although no grammar uses most of them
/// yet, so that the first grammar that needs one does not change the lexical
/// contract — the reasoning of docs/decisions/0006-one-lexer-two-grammars.md,
/// applied to a lexer of its own. There is no line ending: C does not care.
enum class TokenKind : std::uint8_t
{
  END_OF_FILE,
  COMMENT,

  /// Input the lexer rejected. Already diagnosed: a grammar must stay quiet
  /// about it rather than adding a parse error on top.
  UNKNOWN,

  IDENTIFIER,
  KEYWORD,
  INTEGER_CONSTANT,

  /// `"..."` and `'.'`, perhaps with a Charset's name glued to the quote,
  /// which is part of the token — see
  /// docs/decisions/0095-literals-and-the-runtime.md.
  STRING_LITERAL,
  CHARACTER_CONSTANT,

  LEFT_BRACKET,
  RIGHT_BRACKET,
  LEFT_PAREN,
  RIGHT_PAREN,
  LEFT_BRACE,
  RIGHT_BRACE,
  DOT,
  ARROW,
  PLUS_PLUS,
  MINUS_MINUS,
  AMPERSAND,
  STAR,
  PLUS,
  MINUS,
  TILDE,
  BANG,
  SLASH,
  PERCENT,
  LESS_LESS,
  GREATER_GREATER,
  LESS,
  GREATER,
  LESS_EQUAL,
  GREATER_EQUAL,
  EQUAL_EQUAL,
  BANG_EQUAL,
  CARET,
  PIPE,
  AMPERSAND_AMPERSAND,
  PIPE_PIPE,
  QUESTION,
  COLON,
  COLON_COLON,
  SEMICOLON,
  ELLIPSIS,
  EQUAL,
  STAR_EQUAL,
  SLASH_EQUAL,
  PERCENT_EQUAL,
  PLUS_EQUAL,
  MINUS_EQUAL,
  LESS_LESS_EQUAL,
  GREATER_GREATER_EQUAL,
  AMPERSAND_EQUAL,
  CARET_EQUAL,
  PIPE_EQUAL,
  COMMA,
  HASH,
  HASH_HASH,
};

std::string_view nameOf( TokenKind kind );

/// Every word the subset reserves: the keywords of C23, including those of
/// constructs the subset refuses, and its own type names — see
/// docs/decisions/0071-the-subsets-spelling.md.
///
/// Four are named after what they are rather than how they are spelled,
/// because `<windows.h>` defines `TRUE`, `FALSE`, `VOID` and `CONST` as macros.
enum class Keyword : std::uint8_t
{
  NONE,

  ALIGNAS,
  ALIGNOF,
  AUTO,
  BOOL,
  BREAK,
  CASE,
  CHAR,
  CONST_QUALIFIER,
  CONSTEXPR,
  CONTINUE,
  DEFAULT,
  DO,
  DOUBLE,
  ELSE,
  ENUM,
  EXTERN,
  FALSE_CONSTANT,
  FLOAT,
  FOR,
  GOTO,
  IF,
  INLINE,
  INT,
  LONG,
  NULLPTR,
  REGISTER,
  RESTRICT,
  RETURN,
  SHORT,
  SIGNED,
  SIZEOF,
  STATIC,
  STATIC_ASSERT,
  STRUCT,
  SWITCH,
  THREAD_LOCAL,
  TRUE_CONSTANT,
  TYPEDEF,
  TYPEOF,
  TYPEOF_UNQUAL,
  UNION,
  UNSIGNED,
  VOID_TYPE,
  VOLATILE,
  WHILE,
  ATOMIC,
  BIT_INT,
  COMPLEX,
  DECIMAL128,
  DECIMAL32,
  DECIMAL64,
  GENERIC,
  IMAGINARY,
  NORETURN,

  U8,
  I8,
  U16,
  I16,
};

/// The keyword an identifier's spelling is, or NONE. C23's alternative
/// spellings — `_Bool`, `_Alignas` and the rest — are the keyword they spell.
Keyword keywordOf( std::string_view spelling );

/// Position and length rather than text, as syntax::Token is: the text is
/// recoverable from the SourceManager.
struct Token
{
  TokenKind kind = TokenKind::END_OF_FILE;

  /// NONE on every token that is not a KEYWORD.
  Keyword keyword = Keyword::NONE;

  diag::SourceLocation location;
  std::uint32_t length = 0;

  [[nodiscard]] diag::SourceSpan span() const
  {
    return diag::SourceSpan{ .begin = location, .length = length };
  }
};

static_assert( sizeof( Token ) == 12, "Token must stay twelve bytes wide" );

} // namespace nga::c
