#include "nga/syntax/Lexer.hpp"

#include "nga/syntax/Literal.hpp"

#include <algorithm>
#include <cstddef>
#include <string>
#include <utility>

namespace nga::syntax
{

namespace
{

bool isIdentifierStart( char c )
{
  return ( c >= 'A' && c <= 'Z' ) || ( c >= 'a' && c <= 'z' ) || c == '_';
}

bool isDigit( char c )
{
  return c >= '0' && c <= '9';
}

bool isIdentifierContinue( char c )
{
  return isIdentifierStart( c ) || isDigit( c );
}

/// Anything that could plausibly have been meant as a digit. Used so that
/// `$FG` and `%1012` report the offending character instead of ending the
/// literal early and leaving a stray identifier behind.
bool isDigitLike( char c )
{
  return isIdentifierContinue( c ) && c != '_';
}

int valueOfDigit( char c )
{
  if ( isDigit( c ) )
  {
    return c - '0';
  }
  if ( c >= 'A' && c <= 'F' )
  {
    return c - 'A' + 10;
  }
  if ( c >= 'a' && c <= 'f' )
  {
    return c - 'a' + 10;
  }
  return -1;
}

std::string_view nameOfBase( char prefix )
{
  switch ( prefix )
  {
  case '$':
    return "hexadecimal";
  case '%':
    return "binary";
  default:
    return "decimal";
  }
}

} // namespace

Lexer::Lexer( diag::SourceManager const& sources, diag::FileId file, diag::DiagnosticSink& sink )
    : mSources( &sources ), mSink( &sink ), mFile( file ), mText( sources.contentsOf( file ) )
{
  // A byte order mark carries no information NGA acts on, and every later rule
  // is simpler for never seeing it.
  if ( mText.starts_with( "\xEF\xBB\xBF" ) )
  {
    mPos = 3;
  }
}

char Lexer::peek( std::uint32_t ahead ) const
{
  std::uint32_t const at = mPos + ahead;
  return at < mText.size() ? mText[at] : '\0';
}

Token Lexer::make( TokenKind kind, std::uint32_t begin, bool startsLine, Direction direction )
{
  mPrevious = kind;
  mAtLineStart = kind == TokenKind::LINE_END;

  return Token{ .kind = kind,
                .startsLine = startsLine,
                .direction = direction,
                .location = mSources->locationOf( mFile, begin ),
                .length = mPos - begin };
}

void Lexer::report( diag::Diagnostic value, std::uint32_t begin, std::uint32_t length )
{
  mSink->add( std::move( value ).at( mSources->locationOf( mFile, begin ), length ) );
}

bool Lexer::skipBlanks()
{
  std::uint32_t const begin = mPos;
  while ( !atEnd() && ( peek() == ' ' || peek() == '\t' ) )
  {
    ++mPos;
  }
  return mPos != begin;
}

Token Lexer::next()
{
  bool const sawBlanks = skipBlanks();
  bool const startsLine = mAtLineStart && !sawBlanks;
  std::uint32_t const begin = mPos;

  if ( atEnd() )
  {
    if ( mPrevious == TokenKind::LINE_END || mPrevious == TokenKind::END_OF_FILE )
    {
      return make( TokenKind::END_OF_FILE, begin, startsLine );
    }
    return make( TokenKind::LINE_END, begin, startsLine );
  }

  char const c = peek();
  if ( c == '\n' || c == '\r' )
  {
    return lexLineEnd( begin, startsLine );
  }
  if ( c == ';' )
  {
    return lexComment( begin, startsLine );
  }
  if ( c == '@' )
  {
    return lexLocalLabel( begin, startsLine );
  }
  if ( c == '"' || c == '\'' )
  {
    return lexQuoted( begin, startsLine );
  }
  if ( c == '$' || c == '%' || isDigit( c ) )
  {
    return lexNumber( begin, startsLine );
  }
  if ( isIdentifierStart( c ) )
  {
    return lexIdentifier( begin, startsLine );
  }
  return lexPunctuation( begin, startsLine );
}

Token Lexer::lexLineEnd( std::uint32_t begin, bool startsLine )
{
  if ( peek() == '\r' && peek( 1 ) == '\n' )
  {
    ++mPos;
  }
  ++mPos;
  return make( TokenKind::LINE_END, begin, startsLine );
}

Token Lexer::lexComment( std::uint32_t begin, bool startsLine )
{
  ++mPos;
  bool reportedNonAscii = false;
  while ( !atEnd() && peek() != '\n' && peek() != '\r' )
  {
    consumeTextCharacter( reportedNonAscii );
  }
  return make( TokenKind::COMMENT, begin, startsLine );
}

Token Lexer::lexIdentifier( std::uint32_t begin, bool startsLine )
{
  while ( !atEnd() && isIdentifierContinue( peek() ) )
  {
    ++mPos;
  }

  // An identifier written directly against a quote is the literal's character
  // set prefix, and the literal starts where the identifier does.
  if ( peek() == '"' || peek() == '\'' )
  {
    return lexQuoted( begin, startsLine );
  }
  return make( TokenKind::IDENTIFIER, begin, startsLine );
}

Token Lexer::lexLocalLabel( std::uint32_t begin, bool startsLine )
{
  ++mPos;

  Direction direction = Direction::NONE;
  if ( peek() == '+' || peek() == '-' )
  {
    direction = peek() == '+' ? Direction::FORWARD : Direction::BACKWARD;
    ++mPos;
  }

  // Digits are rejected with a message of their own rather than left to lex as
  // `@-` followed by a stray number, which is the mistake anyone arriving from
  // GNU as makes first.
  if ( isDigit( peek() ) )
  {
    while ( !atEnd() && isIdentifierContinue( peek() ) )
    {
      ++mPos;
    }
    report( diagnostic( diag::DiagnosticId::NUMERIC_LOCAL_LABEL ), begin, mPos - begin );
    return make( TokenKind::UNKNOWN, begin, startsLine, direction );
  }

  if ( isIdentifierStart( peek() ) )
  {
    while ( !atEnd() && isIdentifierContinue( peek() ) )
    {
      ++mPos;
    }
    return make( TokenKind::LOCAL_IDENTIFIER, begin, startsLine, direction );
  }
  return make( TokenKind::ANONYMOUS_LABEL, begin, startsLine, direction );
}

Token Lexer::lexNumber( std::uint32_t begin, bool startsLine )
{
  char const prefix = peek() == '$' || peek() == '%' ? peek() : '\0';
  int base = 10;
  if ( prefix == '$' )
  {
    base = 16;
  }
  else if ( prefix == '%' )
  {
    base = 2;
  }
  if ( prefix != '\0' )
  {
    ++mPos;
  }

  // One literal is one mistake: `123abc` and `1___0` each report once, however
  // many characters carry the fault.
  std::uint32_t digits = 0;
  bool reportedDigit = false;
  bool reportedSeparator = false;

  while ( !atEnd() )
  {
    char const c = peek();
    if ( c == '_' )
    {
      if ( ( digits == 0 || !isDigitLike( peek( 1 ) ) ) && !std::exchange( reportedSeparator, true ) )
      {
        report( diagnostic( diag::DiagnosticId::MISPLACED_DIGIT_SEPARATOR ), mPos, 1 );
      }
      ++mPos;
      continue;
    }
    if ( !isDigitLike( c ) )
    {
      break;
    }

    // `0x1F` is a decimal zero followed by an identifier under every rule this
    // lexer has; saying so plainly beats reporting `x` as an invalid digit.
    if ( base == 10 && digits == 1 && mText[begin] == '0' && ( c == 'x' || c == 'X' ) )
    {
      while ( !atEnd() && isDigitLike( peek() ) )
      {
        ++mPos;
      }
      report( diagnostic( diag::DiagnosticId::HEX_PREFIX_NOT_SUPPORTED ), begin, mPos - begin );
      return make( TokenKind::UNKNOWN, begin, startsLine );
    }

    int const value = valueOfDigit( c );
    if ( ( value < 0 || value >= base ) && !std::exchange( reportedDigit, true ) )
    {
      report( diagnostic( diag::DiagnosticId::INVALID_DIGIT )
                  .arg( "character", std::string{ c } )
                  .arg( "base", std::string{ nameOfBase( prefix ) } ),
              mPos,
              1 );
    }
    ++digits;
    ++mPos;
  }

  if ( digits == 0 )
  {
    report(
        diagnostic( diag::DiagnosticId::EMPTY_NUMBER ).arg( "prefix", std::string{ prefix } ), begin, mPos - begin );
    return make( TokenKind::UNKNOWN, begin, startsLine );
  }

  if ( reportedDigit || reportedSeparator )
  {
    // Already diagnosed. Marked as rejected so that a grammar stays quiet about
    // it and nothing downstream ever asks a malformed literal for its value.
    return make( TokenKind::UNKNOWN, begin, startsLine );
  }

  // Whether a value fits where it is used belongs to the layer that evaluates
  // it. Whether it can be represented at all belongs here, and settling it here
  // is what makes converting a NUMBER token to a value a total operation.
  if ( !numericValueOf( mText.substr( begin, mPos - begin ) ).has_value() )
  {
    report( diagnostic( diag::DiagnosticId::NUMBER_TOO_LARGE ), begin, mPos - begin );
    return make( TokenKind::UNKNOWN, begin, startsLine );
  }

  return make( TokenKind::NUMBER, begin, startsLine );
}

void Lexer::consumeTextCharacter( bool& reportedInvalid )
{
  std::uint32_t const length = utf8LengthAt( mText, mPos );
  if ( length == 0 )
  {
    if ( !reportedInvalid )
    {
      report( diagnostic( diag::DiagnosticId::INVALID_UTF8 ), mPos, 1 );
      reportedInvalid = true;
    }
    ++mPos;
    return;
  }

  mPos += length;
}

Token Lexer::lexQuoted( std::uint32_t begin, bool startsLine )
{
  char const quote = peek();
  bool const isString = quote == '"';
  ++mPos;

  std::uint32_t characters = 0;
  bool reportedEncoding = false;
  bool closed = false;

  while ( !atEnd() )
  {
    char const c = peek();
    if ( c == '\n' || c == '\r' )
    {
      break;
    }
    if ( c == quote )
    {
      ++mPos;
      closed = true;
      break;
    }

    if ( c == '\\' )
    {
      char const escape = peek( 1 );

      // A backslash last in the file, or against a line ending, escapes
      // nothing: there are no line continuations, and consuming the line
      // ending here would hide it from the grammar and swallow the next line.
      if ( mPos + 1 >= mText.size() || escape == '\n' || escape == '\r' )
      {
        ++mPos;
        break;
      }

      if ( escape != '\\' && escape != '"' && escape != '\'' && escape != 'n' && escape != 't' && escape != '0' )
      {
        report( diagnostic( diag::DiagnosticId::UNKNOWN_ESCAPE ).arg( "escape", std::string{ escape } ), mPos, 2 );
      }
      mPos += 2;
      ++characters;
      continue;
    }

    consumeTextCharacter( reportedEncoding );
    ++characters;
  }

  if ( !closed )
  {
    // A string that ran off the end of its line and one that ran off the end of
    // the file are different mistakes, and the line ending is left unconsumed
    // so that recovery resumes on the next line rather than swallowing it.
    auto id = diag::DiagnosticId::UNTERMINATED_CHARACTER;
    if ( isString )
    {
      id = atEnd() ? diag::DiagnosticId::UNTERMINATED_STRING : diag::DiagnosticId::LINE_END_IN_STRING;
    }
    report( diagnostic( id ), begin, mPos - begin );
  }

  if ( !isString && closed )
  {
    if ( characters == 0 )
    {
      report( diagnostic( diag::DiagnosticId::EMPTY_CHARACTER ), begin, mPos - begin );
    }
    else if ( characters > 1 )
    {
      report( diagnostic( diag::DiagnosticId::MULTI_CHARACTER ), begin, mPos - begin );
    }
  }

  return make( isString ? TokenKind::STRING : TokenKind::CHARACTER, begin, startsLine );
}

Token Lexer::lexPunctuation( std::uint32_t begin, bool startsLine )
{
  char const c = peek();
  char const after = peek( 1 );

  auto const accept = [&]( TokenKind kind, std::uint32_t length )
  {
    mPos += length;
    return make( kind, begin, startsLine );
  };

  switch ( c )
  {
  case '#':
    return accept( TokenKind::HASH, 1 );
  case '(':
    return accept( TokenKind::LEFT_PAREN, 1 );
  case ')':
    return accept( TokenKind::RIGHT_PAREN, 1 );
  case '[':
    return accept( TokenKind::LEFT_BRACKET, 1 );
  case ']':
    return accept( TokenKind::RIGHT_BRACKET, 1 );
  case '{':
    return accept( TokenKind::LEFT_BRACE, 1 );
  case '}':
    return accept( TokenKind::RIGHT_BRACE, 1 );
  case ',':
    return accept( TokenKind::COMMA, 1 );
  case ':':
    return accept( TokenKind::COLON, 1 );
  case '?':
    return accept( TokenKind::QUESTION, 1 );
  case '+':
    return accept( TokenKind::PLUS, 1 );
  case '-':
    return accept( TokenKind::MINUS, 1 );
  case '*':
    return accept( TokenKind::STAR, 1 );
  case '/':
    return accept( TokenKind::SLASH, 1 );
  case '^':
    return accept( TokenKind::CARET, 1 );
  case '~':
    return accept( TokenKind::TILDE, 1 );
  case '.':
    if ( after == '.' )
    {
      return peek( 2 ) == '.' ? accept( TokenKind::ELLIPSIS, 3 ) : accept( TokenKind::DOT_DOT, 2 );
    }
    return accept( TokenKind::DOT, 1 );
  case '=':
    return after == '=' ? accept( TokenKind::EQUAL_EQUAL, 2 ) : accept( TokenKind::EQUAL, 1 );
  case '!':
    return after == '=' ? accept( TokenKind::BANG_EQUAL, 2 ) : accept( TokenKind::BANG, 1 );
  case '&':
    return after == '&' ? accept( TokenKind::AMPERSAND_AMPERSAND, 2 ) : accept( TokenKind::AMPERSAND, 1 );
  case '|':
    return after == '|' ? accept( TokenKind::PIPE_PIPE, 2 ) : accept( TokenKind::PIPE, 1 );
  case '<':
    if ( after == '<' )
    {
      return accept( TokenKind::LESS_LESS, 2 );
    }
    return after == '=' ? accept( TokenKind::LESS_EQUAL, 2 ) : accept( TokenKind::LESS, 1 );
  case '>':
    if ( after == '>' )
    {
      return accept( TokenKind::GREATER_GREATER, 2 );
    }
    return after == '=' ? accept( TokenKind::GREATER_EQUAL, 2 ) : accept( TokenKind::GREATER, 1 );
  default:
    return lexUnexpected( begin, startsLine );
  }
}

Token Lexer::lexUnexpected( std::uint32_t begin, bool startsLine )
{
  auto const byte = static_cast<unsigned char>( peek() );

  if ( byte < 0x20U || byte == 0x7FU )
  {
    ++mPos;
    report( diagnostic( diag::DiagnosticId::CONTROL_CHARACTER ).arg( "code", std::int64_t{ byte } ), begin, 1 );
    return make( TokenKind::UNKNOWN, begin, startsLine );
  }

  // The whole character is consumed and underlined, not its first byte, so that
  // one stray non-ASCII character produces one finding rather than four.
  std::uint32_t const length = std::max<std::uint32_t>( utf8LengthAt( mText, mPos ), 1 );
  mPos += length;
  report( diagnostic( diag::DiagnosticId::UNEXPECTED_CHARACTER )
              .arg( "character", std::string{ mText.substr( begin, length ) } ),
          begin,
          length );
  return make( TokenKind::UNKNOWN, begin, startsLine );
}

std::vector<Token> tokenize( diag::SourceManager const& sources, diag::FileId file, diag::DiagnosticSink& sink )
{
  Lexer lexer{ sources, file, sink };

  std::vector<Token> tokens;
  for ( ;; )
  {
    Token const token = lexer.next();
    tokens.push_back( token );
    if ( token.kind == TokenKind::END_OF_FILE )
    {
      return tokens;
    }
  }
}

} // namespace nga::syntax
