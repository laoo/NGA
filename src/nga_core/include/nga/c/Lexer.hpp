#pragma once

#include "nga/c/Token.hpp"
#include "nga/diag/DiagnosticSink.hpp"
#include "nga/diag/SourceManager.hpp"

#include <cstdint>
#include <optional>
#include <string_view>
#include <vector>

namespace nga::c
{

/// Turns one `.ngc` file into tokens, reporting every lexical error it meets
/// and carrying on past each of them — the lexical syntax of
/// docs/spec/c-subset.md.
///
/// A lexer of its own and not syntax::Lexer: `;` ends a statement here and
/// begins a comment there, and `0x1F` is the one spelling of a number here that
/// the assembler refuses. Holds no state beyond its position in one file.
class Lexer
{
public:
  Lexer( diag::SourceManager const& sources, diag::FileId file, diag::DiagnosticSink& sink );

  /// The next token. Ends with an end-of-file token, which is then returned
  /// indefinitely.
  Token next();

private:
  [[nodiscard]] bool atEnd() const
  {
    return mPos >= mText.size();
  }

  [[nodiscard]] char peek( std::uint32_t ahead = 0 ) const;

  Token make( TokenKind kind, std::uint32_t begin, Keyword keyword = Keyword::NONE );
  void report( diag::Diagnostic value, std::uint32_t begin, std::uint32_t length );

  void skipWhitespace();

  Token lexLineComment( std::uint32_t begin );
  Token lexBlockComment( std::uint32_t begin );
  Token lexIdentifier( std::uint32_t begin );
  Token lexLiteral( std::uint32_t begin );
  Token lexConstant( std::uint32_t begin );
  Token lexPunctuator( std::uint32_t begin );
  Token lexUnexpected( std::uint32_t begin );

  /// One character of a comment. Invalid UTF-8 is reported here, once per
  /// comment, since `reportedInvalid` latches.
  void consumeTextCharacter( bool& reportedInvalid );

  diag::SourceManager const* mSources;
  diag::DiagnosticSink* mSink;
  diag::FileId mFile;
  std::string_view mText;

  std::uint32_t mPos = 0;
};

/// Every token of a file, comments included, for the suite's markers read
/// them. A grammar drops what it does not want.
std::vector<Token> tokenize( diag::SourceManager const& sources, diag::FileId file, diag::DiagnosticSink& sink );

/// The value of an integer constant.
///
/// Nothing only when the text is not a constant this lexer would accept: a
/// malformed one is rejected where it is read, and so is one too large to be
/// represented, so every INTEGER_CONSTANT token has a value.
std::optional<std::int64_t> valueOfIntegerConstant( std::string_view text );

} // namespace nga::c
