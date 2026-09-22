#pragma once

#include "nga/diag/DiagnosticSink.hpp"
#include "nga/diag/SourceManager.hpp"
#include "nga/syntax/Token.hpp"

#include <cstdint>
#include <string_view>
#include <vector>

namespace nga::syntax
{

/// Turns one file into tokens, reporting every lexical error it meets and
/// carrying on past each of them.
///
/// Holds no state beyond its position in one file, so Modules lex concurrently
/// with nothing shared but the SourceManager, which is read-only here.
class Lexer
{
public:
  Lexer( diag::SourceManager const& sources, diag::FileId file, diag::DiagnosticSink& sink );

  /// The next token. Ends with a line ending and an end-of-file token, which is
  /// then returned indefinitely.
  Token next();

private:
  [[nodiscard]] bool atEnd() const
  {
    return mPos >= mText.size();
  }

  [[nodiscard]] char peek( std::uint32_t ahead = 0 ) const;

  Token make( TokenKind kind, std::uint32_t begin, bool startsLine, Direction direction = Direction::NONE );
  void report( diag::Diagnostic value, std::uint32_t begin, std::uint32_t length );

  bool skipBlanks();

  Token lexLineEnd( std::uint32_t begin, bool startsLine );
  Token lexComment( std::uint32_t begin, bool startsLine );
  Token lexIdentifier( std::uint32_t begin, bool startsLine );
  Token lexLocalLabel( std::uint32_t begin, bool startsLine );
  Token lexNumber( std::uint32_t begin, bool startsLine );
  Token lexQuoted( std::uint32_t begin, bool startsLine );
  Token lexPunctuation( std::uint32_t begin, bool startsLine );
  Token lexUnexpected( std::uint32_t begin, bool startsLine );

  /// Consumes one character of text content, reporting ill-formed UTF-8 and,
  /// when `plainAscii`, anything outside ASCII. `reportedNonAscii` latches, so
  /// a literal complains about its encoding at most once.
  /// One character of a comment or a literal. Invalid UTF-8 is reported here
  /// because it is a fact about the bytes; whether a non-ASCII character is
  /// allowed is a fact about where the literal stands, and belongs above.
  void consumeTextCharacter( bool& reportedInvalid );

  diag::SourceManager const* mSources;
  diag::DiagnosticSink* mSink;
  diag::FileId mFile;
  std::string_view mText;

  std::uint32_t mPos = 0;
  bool mAtLineStart = true;
  TokenKind mPrevious = TokenKind::LINE_END;
};

/// Every token of a file, including comments and line endings. A grammar reads
/// this through whatever filter it needs.
std::vector<Token> tokenize( diag::SourceManager const& sources, diag::FileId file, diag::DiagnosticSink& sink );

} // namespace nga::syntax
