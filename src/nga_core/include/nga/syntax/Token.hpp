#pragma once

#include "nga/diag/SourceLocation.hpp"

#include <cstdint>
#include <string_view>

namespace nga::syntax
{

/// The lexical categories of docs/spec/lexical-syntax.md.
///
/// Deliberately coarse where a grammar can do better: one NUMBER covers all
/// three bases, and no kind distinguishes a directive from a qualified name,
/// because DOT is never resolved by the lexer.
enum class TokenKind : std::uint8_t
{
  END_OF_FILE,
  LINE_END,
  COMMENT,

  /// Input the lexer rejected. Already diagnosed: a grammar must stay quiet
  /// about it rather than adding a parse error on top.
  UNKNOWN,

  IDENTIFIER,
  LOCAL_IDENTIFIER, ///< `@name`, with a direction
  ANONYMOUS_LABEL,  ///< `@`, with a direction
  NUMBER,
  CHARACTER,
  STRING,

  HASH,
  LEFT_PAREN,
  RIGHT_PAREN,
  LEFT_BRACKET,
  RIGHT_BRACKET,
  LEFT_BRACE,
  RIGHT_BRACE,
  COMMA,
  COLON,
  QUESTION,
  DOT,
  DOT_DOT,
  ELLIPSIS, ///< `...`, which spreads a pack or marks a parameter as one

  EQUAL,
  EQUAL_EQUAL,
  BANG,
  BANG_EQUAL,
  LESS,
  LESS_EQUAL,
  LESS_LESS,
  GREATER,
  GREATER_EQUAL,
  GREATER_GREATER,
  AMPERSAND,
  AMPERSAND_AMPERSAND,
  PIPE,
  PIPE_PIPE,

  PLUS,
  MINUS,
  STAR,
  SLASH,
  CARET,
  TILDE,
};

std::string_view nameOf( TokenKind kind );

/// Which definition a local label reference selects. NONE on every token that
/// is not a local label, and on a bare `@name` that must resolve uniquely.
enum class Direction : std::uint8_t
{
  NONE,
  FORWARD,
  BACKWARD,
};

/// Position and length rather than text: a token is produced for every byte of
/// every Module, and the text is recoverable from the SourceManager.
struct Token
{
  TokenKind kind = TokenKind::END_OF_FILE;

  /// First token of its line and at column one, which is what marks a label
  /// definition. Recorded here rather than recomputed from a position, so
  /// that nothing downstream has to look at text to know it.
  bool startsLine = false;

  Direction direction = Direction::NONE;

  diag::SourceLocation location;
  std::uint32_t length = 0;

  [[nodiscard]] diag::SourceSpan span() const
  {
    return diag::SourceSpan{ .begin = location, .length = length };
  }
};

static_assert( sizeof( Token ) == 12, "Token must stay twelve bytes wide" );

} // namespace nga::syntax
