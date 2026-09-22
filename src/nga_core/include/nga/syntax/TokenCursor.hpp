#pragma once

#include "nga/syntax/Token.hpp"

#include <cstdint>
#include <span>

namespace nga::syntax
{

/// A grammar's filtered view of the token stream, and the only thing that ever
/// touches tokens.
///
/// [0006](docs/decisions/0006-one-lexer-two-grammars.md) puts the filter in one
/// place rather than scattering it through a parser: comments are always
/// dropped, and line endings are dropped by the Project file's grammar and kept
/// by the assembler's. Being the sole reader is also what makes splicing a
/// macro expansion a change in this class rather than everywhere.
///
/// Lookahead is bounded and forward. Nothing rewinds, so a parser cannot
/// backtrack by accident.
class TokenCursor
{
public:
  explicit TokenCursor( std::span<Token const> tokens, bool skipLineEnds = false );

  /// The token `ahead` retained tokens from here. Past the end it repeats the
  /// end-of-file token, so a caller never has to check a bound.
  [[nodiscard]] Token const& peek( std::uint32_t ahead = 0 ) const;

  [[nodiscard]] Token const& current() const
  {
    return peek();
  }

  [[nodiscard]] TokenKind kind() const
  {
    return peek().kind;
  }

  [[nodiscard]] bool at( TokenKind wanted ) const
  {
    return peek().kind == wanted;
  }

  /// True at a line ending or at end of file. A grammar that keeps line endings
  /// asks this rather than comparing two kinds everywhere.
  [[nodiscard]] bool atLineEnd() const;

  [[nodiscard]] bool atEnd() const
  {
    return peek().kind == TokenKind::END_OF_FILE;
  }

  Token advance();

  /// Consumes one token of `wanted` and says whether it did.
  bool match( TokenKind wanted );

  /// Discards everything up to and including the next line ending, which is the
  /// recovery point line-oriented syntax gives for free. Stops at end of file.
  void skipToNextLine();

private:
  [[nodiscard]] bool isRetained( Token const& token ) const;
  void settle();

  std::span<Token const> mTokens;
  bool mSkipLineEnds = false;
  std::uint32_t mIndex = 0;
};

} // namespace nga::syntax
