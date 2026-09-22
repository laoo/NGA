#include "nga/c/Lexer.hpp"

#include "nga/syntax/Literal.hpp"

#include <algorithm>
#include <array>
#include <limits>
#include <string>
#include <utility>

namespace nga::c
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

std::string_view nameOfBase( int base )
{
  switch ( base )
  {
  case 16:
    return "hexadecimal";
  case 2:
    return "binary";
  default:
    return "decimal";
  }
}

/// The prefix of a constant and the base it names: `0x` and `0b` in either
/// case, and none for decimal.
std::pair<int, std::uint32_t> baseOf( std::string_view text )
{
  if ( text.size() >= 2 && text[0] == '0' && ( text[1] == 'x' || text[1] == 'X' ) )
  {
    return { 16, 2 };
  }
  if ( text.size() >= 2 && text[0] == '0' && ( text[1] == 'b' || text[1] == 'B' ) )
  {
    return { 2, 2 };
  }
  return { 10, 0 };
}

/// Whether `text` is one of C23's integer suffixes: `u`, `l`, `ll` or `wb`,
/// each in one case, and `u` beside any of the others in either order. Told
/// apart from a stray letter so that the finding says what was meant.
bool isIntegerSuffix( std::string_view text )
{
  static constexpr std::array<std::string_view, 3> UNSIGNED = { "", "u", "U" };
  static constexpr std::array<std::string_view, 7> WIDTH = { "", "l", "L", "ll", "LL", "wb", "WB" };
  for ( std::string_view const u : UNSIGNED )
  {
    for ( std::string_view const width : WIDTH )
    {
      if ( u.empty() && width.empty() )
      {
        continue;
      }
      if ( text == std::string{ u } + std::string{ width } || text == std::string{ width } + std::string{ u } )
      {
        return true;
      }
    }
  }
  return false;
}

} // namespace

std::optional<std::int64_t> valueOfIntegerConstant( std::string_view text )
{
  auto const [base, prefix] = baseOf( text );
  if ( base == 10 && text.size() > 1 && text[0] == '0' )
  {
    return std::nullopt;
  }

  std::int64_t value = 0;
  std::uint32_t digits = 0;
  for ( char const c : text.substr( prefix ) )
  {
    if ( c == '\'' )
    {
      continue;
    }
    int const digit = valueOfDigit( c );
    if ( digit < 0 || digit >= base )
    {
      return std::nullopt;
    }
    if ( value > ( ( std::numeric_limits<std::int64_t>::max() - digit ) / base ) )
    {
      return std::nullopt;
    }
    value = ( value * base ) + digit;
    ++digits;
  }
  return digits == 0 ? std::nullopt : std::optional{ value };
}

Lexer::Lexer( diag::SourceManager const& sources, diag::FileId file, diag::DiagnosticSink& sink )
    : mSources( &sources ), mSink( &sink ), mFile( file ), mText( sources.contentsOf( file ) )
{
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

Token Lexer::make( TokenKind kind, std::uint32_t begin, Keyword keyword )
{
  return Token{
    .kind = kind, .keyword = keyword, .location = mSources->locationOf( mFile, begin ), .length = mPos - begin
  };
}

void Lexer::report( diag::Diagnostic value, std::uint32_t begin, std::uint32_t length )
{
  mSink->add( std::move( value ).at( mSources->locationOf( mFile, begin ), length ) );
}

void Lexer::skipWhitespace()
{
  // Space, tab and the line endings, and nothing else: a form feed or a
  // vertical tab would mean nothing to a program, and refusing them is what
  // stops a binary file handed over by mistake from lexing into a column of
  // tokens.
  while ( !atEnd() && ( peek() == ' ' || peek() == '\t' || peek() == '\n' || peek() == '\r' ) )
  {
    ++mPos;
  }
}

Token Lexer::next()
{
  skipWhitespace();
  std::uint32_t const begin = mPos;

  if ( atEnd() )
  {
    return make( TokenKind::END_OF_FILE, begin );
  }

  char const c = peek();
  if ( c == '/' && peek( 1 ) == '/' )
  {
    return lexLineComment( begin );
  }
  if ( c == '/' && peek( 1 ) == '*' )
  {
    return lexBlockComment( begin );
  }
  if ( isDigit( c ) || ( c == '.' && isDigit( peek( 1 ) ) ) )
  {
    return lexConstant( begin );
  }
  if ( isIdentifierStart( c ) )
  {
    return lexIdentifier( begin );
  }
  if ( c == '"' || c == '\'' )
  {
    return lexLiteral( begin );
  }
  return lexPunctuator( begin );
}

Token Lexer::lexLineComment( std::uint32_t begin )
{
  mPos += 2;
  bool reportedInvalid = false;
  while ( !atEnd() && peek() != '\n' && peek() != '\r' )
  {
    consumeTextCharacter( reportedInvalid );
  }

  // C joins a line ending in a backslash to the next before it looks for
  // comments, so the next line would be comment too. Nothing is joined here,
  // and a program C would read differently is refused rather than compiled.
  if ( !atEnd() && mPos > begin + 2 && mText[mPos - 1] == '\\' )
  {
    report( diagnostic( diag::DiagnosticId::C_LINE_SPLICE ), mPos - 1, 1 );
  }
  return make( TokenKind::COMMENT, begin );
}

Token Lexer::lexBlockComment( std::uint32_t begin )
{
  mPos += 2;
  bool reportedInvalid = false;
  while ( !atEnd() )
  {
    if ( peek() == '*' && peek( 1 ) == '/' )
    {
      mPos += 2;
      return make( TokenKind::COMMENT, begin );
    }
    consumeTextCharacter( reportedInvalid );
  }
  report( diagnostic( diag::DiagnosticId::C_UNTERMINATED_COMMENT ), begin, 2 );
  return make( TokenKind::COMMENT, begin );
}

void Lexer::consumeTextCharacter( bool& reportedInvalid )
{
  std::uint32_t const length = syntax::utf8LengthAt( mText, mPos );
  if ( length == 0 )
  {
    if ( !std::exchange( reportedInvalid, true ) )
    {
      report( diagnostic( diag::DiagnosticId::C_INVALID_UTF8 ), mPos, 1 );
    }
    ++mPos;
    return;
  }
  mPos += length;
}

Token Lexer::lexIdentifier( std::uint32_t begin )
{
  while ( !atEnd() && isIdentifierContinue( peek() ) )
  {
    ++mPos;
  }
  // A name glued to a quote is a literal's Charset, and one token with it, as
  // C's own `u8"x"` is — see docs/decisions/0095-literals-and-the-runtime.md.
  if ( peek() == '"' || peek() == '\'' )
  {
    return lexLiteral( begin );
  }
  std::string_view const spelling = mText.substr( begin, mPos - begin );
  Keyword const keyword = keywordOf( spelling );

  // C keeps these names for the implementation, and the compiler takes them:
  // a Proc's scratch bytes are `__t0` and on, in its scope, where no name a
  // program writes can then meet one — see
  // docs/decisions/0076-arithmetic-in-the-subset.md. C23's `_Bool` and the
  // rest are keywords, and were found above.
  bool const reserved = spelling.size() >= 2 && spelling[0] == '_' &&
                        ( spelling[1] == '_' || ( spelling[1] >= 'A' && spelling[1] <= 'Z' ) );
  if ( keyword == Keyword::NONE && reserved )
  {
    report( diagnostic( diag::DiagnosticId::C_RESERVED_IDENTIFIER ).arg( "name", std::string{ spelling } ),
            begin,
            mPos - begin );
    return make( TokenKind::UNKNOWN, begin );
  }
  return make( keyword == Keyword::NONE ? TokenKind::IDENTIFIER : TokenKind::KEYWORD, begin, keyword );
}

Token Lexer::lexLiteral( std::uint32_t begin )
{
  bool const prefixed = mPos > begin;
  char const quote = peek();
  bool const character = quote == '\'';
  ++mPos;
  std::uint32_t characters = 0;
  bool rejected = false;
  while ( true )
  {
    if ( atEnd() || peek() == '\n' || peek() == '\r' )
    {
      report( diagnostic( diag::DiagnosticId::C_UNTERMINATED_LITERAL )
                  .arg( "what", std::string{ character ? "character constant" : "string literal" } ),
              begin,
              mPos - begin );
      return make( TokenKind::UNKNOWN, begin );
    }
    char const c = peek();
    if ( c == quote )
    {
      ++mPos;
      break;
    }
    if ( c == '\\' )
    {
      char const escaped = peek( 1 );
      // The assembler's six, each one ASCII character the Charset translates.
      if ( escaped != '\\' && escaped != '"' && escaped != '\'' && escaped != 'n' && escaped != 't' && escaped != '0' )
      {
        std::uint32_t const length = escaped == '\n' || escaped == '\r' || escaped == '\0' ? 1U : 2U;
        report( diagnostic( diag::DiagnosticId::C_UNKNOWN_ESCAPE )
                    .arg( "escape", std::string{ mText.substr( mPos, length ) } ),
                mPos,
                length );
        rejected = true;
        mPos += length == 2 ? 2U : 1U;
        ++characters;
        continue;
      }
      mPos += 2;
      ++characters;
      continue;
    }
    std::uint32_t const length = syntax::utf8LengthAt( mText, mPos );
    if ( length == 0 )
    {
      report( diagnostic( diag::DiagnosticId::C_INVALID_UTF8 ), mPos, 1 );
      rejected = true;
      ++mPos;
      continue;
    }
    // An unprefixed literal is ASCII, untranslated: a character beyond it
    // wanted a Charset nobody named.
    if ( !prefixed && ( static_cast<unsigned char>( c ) & 0x80U ) != 0 )
    {
      report( diagnostic( diag::DiagnosticId::C_NON_ASCII_UNPREFIXED )
                  .arg( "character", std::string{ mText.substr( mPos, length ) } ),
              mPos,
              length );
      rejected = true;
    }
    mPos += length;
    ++characters;
  }
  if ( character && characters != 1 )
  {
    report( diagnostic( diag::DiagnosticId::C_CHARACTER_COUNT )
                .arg( "what", std::string{ characters == 0 ? "no character" : "more than one" } ),
            begin,
            mPos - begin );
    rejected = true;
  }
  if ( rejected )
  {
    return make( TokenKind::UNKNOWN, begin );
  }
  return make( character ? TokenKind::CHARACTER_CONSTANT : TokenKind::STRING_LITERAL, begin );
}

Token Lexer::lexConstant( std::uint32_t begin )
{
  // C reads a preprocessing number greedily and only then asks whether it is
  // a constant (C23 6.4.8), so `123abc` and `0x1e+1` are each one token. The
  // same span is read here, which is what makes a malformed constant one
  // finding and never a constant followed by a name. A separator is taken
  // wherever it stands, so that one misplaced says so rather than lexing as a
  // stray character.
  while ( !atEnd() )
  {
    char const c = peek();
    if ( ( c == 'e' || c == 'E' || c == 'p' || c == 'P' ) && ( peek( 1 ) == '+' || peek( 1 ) == '-' ) )
    {
      mPos += 2;
      continue;
    }
    if ( !isIdentifierContinue( c ) && c != '.' && c != '\'' )
    {
      break;
    }
    ++mPos;
  }

  std::string_view const text = mText.substr( begin, mPos - begin );
  auto const length = static_cast<std::uint32_t>( text.size() );
  auto const reject = [&]( diag::Diagnostic value, std::uint32_t at, std::uint32_t span )
  {
    report( std::move( value ), at, span );
    return make( TokenKind::UNKNOWN, begin );
  };

  auto const [base, prefix] = baseOf( text );
  std::uint32_t end = prefix;
  while ( end < text.size() &&
          ( ( base == 16 ? valueOfDigit( text[end] ) >= 0 : isDigit( text[end] ) ) || text[end] == '\'' ) )
  {
    ++end;
  }
  std::string_view const digits = text.substr( prefix, end - prefix );
  std::string_view const rest = text.substr( end );

  // Checked first, since a fraction or an exponent is what the rest of the
  // text is for, and naming its first letter an invalid digit would mislead.
  bool const exponent = !rest.empty() && ( base == 16 ? ( rest[0] == 'p' || rest[0] == 'P' )
                                                      : base == 10 && ( rest[0] == 'e' || rest[0] == 'E' ) );
  if ( exponent || std::ranges::find( rest, '.' ) != rest.end() )
  {
    return reject( diagnostic( diag::DiagnosticId::C_FLOATING_CONSTANT ), begin, length );
  }

  if ( digits.find_first_not_of( '\'' ) == std::string_view::npos )
  {
    return reject(
        diagnostic( diag::DiagnosticId::C_EMPTY_CONSTANT ).arg( "prefix", std::string{ text.substr( 0, 2 ) } ),
        begin,
        length );
  }

  if ( base == 10 && digits.size() > 1 && digits[0] == '0' )
  {
    return reject(
        diagnostic( diag::DiagnosticId::C_OCTAL_CONSTANT ).arg( "constant", std::string{ text } ), begin, length );
  }

  for ( std::uint32_t i = 0; i < digits.size(); ++i )
  {
    std::uint32_t const at = begin + prefix + i;
    if ( digits[i] == '\'' && ( i == 0 || i + 1 == digits.size() || digits[i + 1] == '\'' ) )
    {
      return reject( diagnostic( diag::DiagnosticId::C_MISPLACED_DIGIT_SEPARATOR ), at, 1 );
    }
    if ( digits[i] != '\'' && valueOfDigit( digits[i] ) >= base )
    {
      return reject( diagnostic( diag::DiagnosticId::C_INVALID_DIGIT )
                         .arg( "character", std::string{ digits[i] } )
                         .arg( "base", std::string{ nameOfBase( base ) } ),
                     at,
                     1 );
    }
  }

  if ( !rest.empty() )
  {
    auto const restLength = static_cast<std::uint32_t>( rest.size() );
    if ( isIntegerSuffix( rest ) )
    {
      return reject( diagnostic( diag::DiagnosticId::C_INTEGER_SUFFIX ).arg( "suffix", std::string{ rest } ),
                     begin + end,
                     restLength );
    }
    return reject( diagnostic( diag::DiagnosticId::C_INVALID_DIGIT )
                       .arg( "character", std::string{ rest.substr( 0, 1 ) } )
                       .arg( "base", std::string{ nameOfBase( base ) } ),
                   begin + end,
                   1 );
  }

  // Whether a value fits where it is used is the compiler's question. Whether
  // it can be represented at all is settled here, which is what makes reading
  // the value of an INTEGER_CONSTANT total.
  if ( !valueOfIntegerConstant( text ).has_value() )
  {
    return reject( diagnostic( diag::DiagnosticId::C_CONSTANT_TOO_LARGE ), begin, length );
  }

  return make( TokenKind::INTEGER_CONSTANT, begin );
}

Token Lexer::lexPunctuator( std::uint32_t begin )
{
  char const c = peek();
  char const after = peek( 1 );

  auto const accept = [&]( TokenKind kind, std::uint32_t length )
  {
    mPos += length;
    return make( kind, begin );
  };

  // Longest match, and no digraph: `<:` is `<` and `:`, which no program the
  // grammar accepts can hold, so a digraph is refused where C would read it.
  switch ( c )
  {
  case '[':
    return accept( TokenKind::LEFT_BRACKET, 1 );
  case ']':
    return accept( TokenKind::RIGHT_BRACKET, 1 );
  case '(':
    return accept( TokenKind::LEFT_PAREN, 1 );
  case ')':
    return accept( TokenKind::RIGHT_PAREN, 1 );
  case '{':
    return accept( TokenKind::LEFT_BRACE, 1 );
  case '}':
    return accept( TokenKind::RIGHT_BRACE, 1 );
  case '~':
    return accept( TokenKind::TILDE, 1 );
  case '?':
    return accept( TokenKind::QUESTION, 1 );
  case ';':
    return accept( TokenKind::SEMICOLON, 1 );
  case ',':
    return accept( TokenKind::COMMA, 1 );
  case '.':
    return after == '.' && peek( 2 ) == '.' ? accept( TokenKind::ELLIPSIS, 3 ) : accept( TokenKind::DOT, 1 );
  case '-':
    if ( after == '>' )
    {
      return accept( TokenKind::ARROW, 2 );
    }
    if ( after == '-' )
    {
      return accept( TokenKind::MINUS_MINUS, 2 );
    }
    return after == '=' ? accept( TokenKind::MINUS_EQUAL, 2 ) : accept( TokenKind::MINUS, 1 );
  case '+':
    if ( after == '+' )
    {
      return accept( TokenKind::PLUS_PLUS, 2 );
    }
    return after == '=' ? accept( TokenKind::PLUS_EQUAL, 2 ) : accept( TokenKind::PLUS, 1 );
  case '&':
    if ( after == '&' )
    {
      return accept( TokenKind::AMPERSAND_AMPERSAND, 2 );
    }
    return after == '=' ? accept( TokenKind::AMPERSAND_EQUAL, 2 ) : accept( TokenKind::AMPERSAND, 1 );
  case '|':
    if ( after == '|' )
    {
      return accept( TokenKind::PIPE_PIPE, 2 );
    }
    return after == '=' ? accept( TokenKind::PIPE_EQUAL, 2 ) : accept( TokenKind::PIPE, 1 );
  case '*':
    return after == '=' ? accept( TokenKind::STAR_EQUAL, 2 ) : accept( TokenKind::STAR, 1 );
  case '/':
    return after == '=' ? accept( TokenKind::SLASH_EQUAL, 2 ) : accept( TokenKind::SLASH, 1 );
  case '%':
    return after == '=' ? accept( TokenKind::PERCENT_EQUAL, 2 ) : accept( TokenKind::PERCENT, 1 );
  case '!':
    return after == '=' ? accept( TokenKind::BANG_EQUAL, 2 ) : accept( TokenKind::BANG, 1 );
  case '=':
    return after == '=' ? accept( TokenKind::EQUAL_EQUAL, 2 ) : accept( TokenKind::EQUAL, 1 );
  case '^':
    return after == '=' ? accept( TokenKind::CARET_EQUAL, 2 ) : accept( TokenKind::CARET, 1 );
  case ':':
    return after == ':' ? accept( TokenKind::COLON_COLON, 2 ) : accept( TokenKind::COLON, 1 );
  case '#':
    return after == '#' ? accept( TokenKind::HASH_HASH, 2 ) : accept( TokenKind::HASH, 1 );
  case '<':
    if ( after == '<' )
    {
      return peek( 2 ) == '=' ? accept( TokenKind::LESS_LESS_EQUAL, 3 ) : accept( TokenKind::LESS_LESS, 2 );
    }
    return after == '=' ? accept( TokenKind::LESS_EQUAL, 2 ) : accept( TokenKind::LESS, 1 );
  case '>':
    if ( after == '>' )
    {
      return peek( 2 ) == '=' ? accept( TokenKind::GREATER_GREATER_EQUAL, 3 ) : accept( TokenKind::GREATER_GREATER, 2 );
    }
    return after == '=' ? accept( TokenKind::GREATER_EQUAL, 2 ) : accept( TokenKind::GREATER, 1 );
  default:
    return lexUnexpected( begin );
  }
}

Token Lexer::lexUnexpected( std::uint32_t begin )
{
  auto const byte = static_cast<unsigned char>( peek() );

  if ( byte < 0x20U || byte == 0x7FU )
  {
    ++mPos;
    report( diagnostic( diag::DiagnosticId::C_CONTROL_CHARACTER ).arg( "code", std::int64_t{ byte } ), begin, 1 );
    return make( TokenKind::UNKNOWN, begin );
  }

  // The whole character is consumed and underlined, not its first byte, so
  // that one stray non-ASCII character produces one finding rather than four.
  std::uint32_t const length = std::max<std::uint32_t>( syntax::utf8LengthAt( mText, mPos ), 1 );
  mPos += length;
  report( diagnostic( diag::DiagnosticId::C_UNEXPECTED_CHARACTER )
              .arg( "character", std::string{ mText.substr( begin, length ) } ),
          begin,
          length );
  return make( TokenKind::UNKNOWN, begin );
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

} // namespace nga::c
