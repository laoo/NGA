#include "nga/syntax/Parser.hpp"

#include "nga/syntax/ExpressionParser.hpp"
#include "nga/syntax/Literal.hpp"

#include <algorithm>
#include <string>
#include <utility>
#include <vector>

namespace nga::syntax
{

namespace
{

bool startsLabelDefinition( Token const& token )
{
  switch ( token.kind )
  {
  case TokenKind::IDENTIFIER:
  case TokenKind::LOCAL_IDENTIFIER:
  case TokenKind::ANONYMOUS_LABEL:
    return token.startsLine;
  default:
    return false;
  }
}

} // namespace

Parser::Parser( diag::SourceManager const& sources, TokenCursor& cursor, Builder& builder, diag::DiagnosticSink& sink )
    : mSources( &sources ), mCursor( &cursor ), mBuilder( &builder ), mSink( &sink )
{
}

void Parser::report( diag::Diagnostic value ) const
{
  mSink->add( std::move( value ) );
}

std::string_view Parser::textOf( Token token ) const
{
  return mSources->textOf( token.span() );
}

std::string Parser::describe( Token token ) const
{
  if ( token.kind == TokenKind::LINE_END || token.kind == TokenKind::END_OF_FILE || token.length == 0 )
  {
    return std::string{ nameOf( token.kind ) };
  }
  return std::string{ textOf( token ) };
}

void Parser::recover()
{
  mCursor->skipToNextLine();
}

void Parser::endStatement( bool quiet )
{
  if ( quiet )
  {
    recover();
    return;
  }
  expectLineEnd();
}

ExpressionPtr Parser::parseExpression()
{
  ExpressionParser expressions{ *mCursor, *mSink };
  return expressions.parse();
}

bool Parser::expectLineEnd()
{
  if ( mCursor->atLineEnd() )
  {
    mCursor->match( TokenKind::LINE_END );
    return true;
  }

  Token const here = mCursor->current();
  report( diag::diagnostic( diag::DiagnosticId::TRAILING_TOKENS )
              .at( here.location, here.length )
              .arg( "token", describe( here ) ) );
  recover();
  return false;
}

void Parser::parseModule()
{
  while ( !mCursor->atEnd() )
  {
    parseLine();
  }

  // Reported where the construct opened rather than at the end of the file:
  // the opening line is where the reader has to go.
  if ( mOpenProc.has_value() )
  {
    report( diag::diagnostic( diag::DiagnosticId::PROC_NOT_CLOSED ).at( mOpenProc->location, mOpenProc->length ) );
  }
  if ( mOpenSection.has_value() )
  {
    report(
        diag::diagnostic( diag::DiagnosticId::SECTION_NOT_CLOSED ).at( mOpenSection->location, mOpenSection->length ) );
  }
  if ( mOpenMacro.has_value() )
  {
    report( diag::diagnostic( diag::DiagnosticId::MACRO_NOT_CLOSED ).at( mOpenMacro->location, mOpenMacro->length ) );
  }
  for ( Token const& open : mOpenNamespaces )
  {
    report( diag::diagnostic( diag::DiagnosticId::NAMESPACE_NOT_CLOSED ).at( open.location, open.length ) );
  }
  for ( OpenConditional const& open : mOpenConditionals )
  {
    report( diag::diagnostic( open.isMatch ? diag::DiagnosticId::MATCH_NOT_CLOSED
                                           : diag::DiagnosticId::CONDITIONAL_NOT_CLOSED )
                .at( open.directive.location, open.directive.length ) );
  }

  // A `.off` with no statement after it silenced nothing, and says so
  // through the same path as one whose statement raised nothing. A `.with`
  // with no statement after it applies to nothing, and is refused.
  flushPendingOffs( std::nullopt );
  if ( mWithPending )
  {
    report( diag::diagnostic( diag::DiagnosticId::WITH_NEEDS_CODE ).at( mWithToken.location, mWithToken.length ) );
    mBuilder->dropWiths();
    mWithPending = false;
  }
  if ( mPendingTaking.has_value() )
  {
    report( diag::diagnostic( diag::DiagnosticId::TAKING_NEEDS_STATEMENT )
                .at( mTakingToken.location, mTakingToken.length )
                .arg( "directive", std::string{ textOf( mTakingToken ) } ) );
    mBuilder->dropTaking();
    mPendingTaking.reset();
  }
  if ( mDeclarePending )
  {
    report( diag::diagnostic( diag::DiagnosticId::DECLARE_NEEDS_TEMPORARY )
                .at( mDeclareToken.location, mDeclareToken.length ) );
    mBuilder->dropDeclaration();
    mDeclarePending = false;
  }
}

void Parser::parseConditional( Token directive )
{
  ExpressionPtr condition = parseExpression();
  bool const quiet = containsError( *condition );
  diag::SourceSpan const span = spanning( directive.span(), condition->span );
  mOpenConditionals.push_back( OpenConditional{ .directive = directive } );
  mBuilder->beginConditional( std::move( condition ), span );
  endStatement( quiet );
}

void Parser::parseBranch( Token directive, bool isElse )
{
  if ( mOpenConditionals.empty() || mOpenConditionals.back().isMatch )
  {
    report( diag::diagnostic( diag::DiagnosticId::UNMATCHED_CONDITIONAL_END )
                .at( directive.location, directive.length )
                .arg( "directive", "." + std::string{ textOf( directive ) } ) );
    recover();
    return;
  }
  if ( mOpenConditionals.back().outside )
  {
    // The `.if` was refused, and a branch of it is part of the same finding.
    recover();
    return;
  }
  if ( mOpenConditionals.back().sawElse )
  {
    // Reported and then taken, so that what follows is still read as a branch
    // rather than as statements with nowhere to belong.
    report( diag::diagnostic( isElse ? diag::DiagnosticId::UNMATCHED_CONDITIONAL_END
                                     : diag::DiagnosticId::ELSIF_AFTER_ELSE )
                .at( directive.location, directive.length )
                .arg( "directive", "." + std::string{ textOf( directive ) } ) );
  }

  ExpressionPtr condition;
  bool quiet = false;
  diag::SourceSpan span = directive.span();
  if ( !isElse )
  {
    condition = parseExpression();
    quiet = containsError( *condition );
    span = spanning( span, condition->span );
  }
  mOpenConditionals.back().sawElse = isElse;
  mBuilder->nextBranch( std::move( condition ), span );
  endStatement( quiet );
}

void Parser::parseConditionalEnd( Token directive, std::string_view spelling )
{
  if ( mOpenConditionals.empty() || mOpenConditionals.back().isMatch )
  {
    report( diag::diagnostic( diag::DiagnosticId::UNMATCHED_CONDITIONAL_END )
                .at( directive.location, directive.length )
                .arg( "directive", "." + std::string{ spelling } ) );
    recover();
    return;
  }
  bool const outside = mOpenConditionals.back().outside;
  mOpenConditionals.pop_back();
  if ( outside )
  {
    // The Builder never saw the `.if`, so it has nothing to close.
    recover();
    return;
  }
  mBuilder->endConditional( directive.span() );
  endStatement( false );
}

bool Parser::isConditionalWord( std::string_view word )
{
  return word == "if" || word == "elsif" || word == "else" || word == "endif" || word == "endi" || word == "match" ||
         word == "case" || word == "endmatch";
}

bool Parser::isDataWord( std::string_view word )
{
  return word == "byte" || word == "word" || word == "hex" || word == "binary" || word == "base64";
}

void Parser::parseMatch( Token directive )
{
  if ( !mOpenMacro.has_value() )
  {
    // A pack is a parameter, and only a body has one.
    report( diag::diagnostic( diag::DiagnosticId::MATCH_OUTSIDE_BODY ).at( directive.location, directive.length ) );
    recover();
    return;
  }
  if ( !mCursor->at( TokenKind::IDENTIFIER ) )
  {
    report( diag::diagnostic( diag::DiagnosticId::EXPECTED_NAME )
                .at( directive.location, directive.length )
                .arg( "after", std::string{ textOf( directive ) } ) );
    recover();
    return;
  }
  Token const subject = mCursor->advance();
  mOpenConditionals.push_back( OpenConditional{ .directive = directive, .isMatch = true } );
  mBuilder->beginMatch( subject, spanning( directive.span(), subject.span() ) );
  expectLineEnd();
}

void Parser::parseCase( Token directive )
{
  if ( mOpenConditionals.empty() || !mOpenConditionals.back().isMatch )
  {
    report( diag::diagnostic( diag::DiagnosticId::CASE_OUTSIDE_MATCH ).at( directive.location, directive.length ) );
    recover();
    return;
  }
  Pattern pattern;
  if ( !parsePattern( pattern, directive ) )
  {
    return;
  }
  diag::SourceSpan const span =
      pattern.names.empty() ? directive.span() : spanning( directive.span(), pattern.names.back().span() );
  mOpenConditionals.back().sawCase = true;
  mBuilder->beginCase( std::move( pattern ), span );
  expectLineEnd();
}

void Parser::parseMatchEnd( Token directive, std::string_view spelling )
{
  if ( mOpenConditionals.empty() || !mOpenConditionals.back().isMatch )
  {
    report( diag::diagnostic( diag::DiagnosticId::UNMATCHED_MATCH_END )
                .at( directive.location, directive.length )
                .arg( "directive", "." + std::string{ spelling } ) );
    recover();
    return;
  }
  if ( !mOpenConditionals.back().sawCase )
  {
    report( diag::diagnostic( diag::DiagnosticId::MATCH_WITHOUT_CASE )
                .at( mOpenConditionals.back().directive.location, mOpenConditionals.back().directive.length ) );
  }
  mOpenConditionals.pop_back();
  mBuilder->endMatch( directive.span() );
  endStatement( false );
}

bool Parser::inBody() const
{
  return mOpenSection.has_value() || mOpenProc.has_value() || mOpenMacro.has_value();
}

bool Parser::inProcNamespace() const
{
  return mOpenProc.has_value() && !mOpenSection.has_value() && !mOpenMacro.has_value() &&
         mOpenNamespaces.size() > mNamespacesAtProc;
}

bool Parser::refusedOutsideBody( Token at, std::string_view what )
{
  // A Namespace in a Proc names what stands beside the Proc; the Proc's own
  // code and data are not in it.
  if ( inProcNamespace() )
  {
    report( diag::diagnostic( diag::DiagnosticId::NAMESPACE_IN_PROC_HOLDS )
                .at( at.location, at.length )
                .arg( "what", std::string{ what } ) );
    recover();
    return true;
  }
  if ( inBody() )
  {
    return false;
  }
  // The `.if` this stands in was the finding; a second one per line inside
  // it would say the same thing again.
  if ( mOpenConditionals.empty() || !mOpenConditionals.back().outside )
  {
    report( diag::diagnostic( diag::DiagnosticId::OUTSIDE_SECTION )
                .at( at.location, at.length )
                .arg( "what", std::string{ what } ) );
  }
  // The line looked like code, so what stood before it was handed to the
  // Builder for it; nothing is going to take it now.
  if ( mWithsHanded )
  {
    mBuilder->dropWiths();
    mWithsHanded = false;
  }
  if ( mTakingHanded )
  {
    mBuilder->dropTaking();
    mTakingHanded = false;
  }
  recover();
  return true;
}

bool Parser::statementAllowed()
{
  if ( mOpenConditionals.empty() || !mOpenConditionals.back().isMatch || mOpenConditionals.back().sawCase )
  {
    return true;
  }
  Token const here = mCursor->current();
  report( diag::diagnostic( diag::DiagnosticId::STATEMENT_BEFORE_CASE ).at( here.location, here.length ) );
  recover();
  return false;
}

bool Parser::nextIsAdjacent() const
{
  Token const& first = mCursor->current();
  Token const& second = mCursor->peek( 1 );
  return second.location.rawOffset() == first.location.rawOffset() + first.length;
}

bool Parser::parsePattern( Pattern& into, Token after )
{
  if ( !mCursor->at( TokenKind::IDENTIFIER ) )
  {
    return true;
  }
  bool misplaced = false;
  while ( true )
  {
    // `name...` is a pack, and only the last name may be one: everything
    // the names before it do not take is what it takes. Reported once and
    // read on, so that the block still opens and closes as one.
    if ( into.pack && !misplaced )
    {
      misplaced = true;
      Token const& late = into.names.back();
      report( diag::diagnostic( diag::DiagnosticId::PACK_NOT_LAST )
                  .at( late.location, late.length )
                  .arg( "name", std::string{ textOf( late ) } ) );
    }
    if ( mCursor->peek( 1 ).kind == TokenKind::ELLIPSIS && nextIsAdjacent() )
    {
      into.names.push_back( mCursor->advance() );
      mCursor->advance(); // `...`
      into.pack = true;
    }
    else
    {
      into.names.push_back( mCursor->advance() );
    }
    if ( !mCursor->at( TokenKind::COMMA ) )
    {
      return true;
    }
    after = mCursor->advance();
    if ( !mCursor->at( TokenKind::IDENTIFIER ) )
    {
      report( diag::diagnostic( diag::DiagnosticId::EXPECTED_NAME )
                  .at( after.location, after.length )
                  .arg( "after", "," ) );
      recover();
      return false;
    }
  }
}

ExpressionPtr Parser::parseItem()
{
  // `rest...`: the pack's name and the dots touching it, which is the
  // whole of the spelling. Anything else before `...` is not a pack.
  if ( mCursor->at( TokenKind::IDENTIFIER ) && mCursor->peek( 1 ).kind == TokenKind::ELLIPSIS && nextIsAdjacent() )
  {
    Token const name = mCursor->advance();
    Token const dots = mCursor->advance();
    return makeExpression( ExpressionKind::SPREAD, name, spanning( name.span(), dots.span() ) );
  }
  ExpressionPtr item = parseExpression();
  if ( mCursor->at( TokenKind::ELLIPSIS ) )
  {
    Token const dots = mCursor->current();
    report( diag::diagnostic( diag::DiagnosticId::SPREAD_NEEDS_NAME ).at( dots.location, dots.length ) );
    item = makeExpression( ExpressionKind::ERROR, dots, item->span );
  }
  return item;
}

bool Parser::atQualifiedUse() const
{
  // `one.fill` at the head of a statement is one unbroken name, as a mnemonic
  // is one word. The dot has to touch both neighbours, which is what tells it
  // from `lda .count`: a mnemonic, then an operand read at the top level. The
  // lexer drops whitespace, so adjacency is read from the tokens' own extents
  // and nothing new is kept for it.
  auto const adjacent = []( Token const& first, Token const& second )
  { return second.location.rawOffset() == first.location.rawOffset() + first.length; };

  return mCursor->at( TokenKind::IDENTIFIER ) && mCursor->peek( 1 ).kind == TokenKind::DOT &&
         mCursor->peek( 2 ).kind == TokenKind::IDENTIFIER && adjacent( mCursor->current(), mCursor->peek( 1 ) ) &&
         adjacent( mCursor->peek( 1 ), mCursor->peek( 2 ) );
}

bool Parser::atOffDirective() const
{
  return mCursor->at( TokenKind::DOT ) && mCursor->peek( 1 ).kind == TokenKind::IDENTIFIER &&
         textOf( mCursor->peek( 1 ) ) == "off";
}

bool Parser::atWithDirective() const
{
  return mCursor->at( TokenKind::DOT ) && mCursor->peek( 1 ).kind == TokenKind::IDENTIFIER &&
         textOf( mCursor->peek( 1 ) ) == "with";
}

bool Parser::atTakingDirective() const
{
  if ( !mCursor->at( TokenKind::DOT ) || mCursor->peek( 1 ).kind != TokenKind::IDENTIFIER )
  {
    return false;
  }
  std::string_view const word = textOf( mCursor->peek( 1 ) );
  return word == "own" || word == "root";
}

bool Parser::atDeclareDirective() const
{
  return mCursor->at( TokenKind::DOT ) && mCursor->peek( 1 ).kind == TokenKind::IDENTIFIER &&
         textOf( mCursor->peek( 1 ) ) == "declare";
}

bool Parser::atTemporaryStatement() const
{
  // A `.ztemp` or a `.temp` carries its name at column one, which is how it is told from
  // every other directive; the name belongs to the statement.
  return startsLabelDefinition( mCursor->current() ) && mCursor->peek( 1 ).kind == TokenKind::DOT &&
         mCursor->peek( 2 ).kind == TokenKind::IDENTIFIER &&
         ( textOf( mCursor->peek( 2 ) ) == "ztemp" || textOf( mCursor->peek( 2 ) ) == "temp" );
}

bool Parser::atInstructionOrData() const
{
  std::uint32_t const start = startsLabelDefinition( mCursor->current() ) ? 1 : 0;
  Token const first = mCursor->peek( start );
  if ( first.kind == TokenKind::IDENTIFIER )
  {
    return true;
  }
  if ( first.kind != TokenKind::DOT || mCursor->peek( start + 1 ).kind != TokenKind::IDENTIFIER )
  {
    return false;
  }
  std::string_view const word = textOf( mCursor->peek( start + 1 ) );
  return word == "byte" || word == "word";
}

void Parser::parseTaking( Token directive, model::Taking kind )
{
  // One taking per statement: an address is followed by this Section or by
  // the hardware, and the first said wins.
  if ( mPendingTaking.has_value() && *mPendingTaking != kind )
  {
    report( diag::diagnostic( diag::DiagnosticId::OWN_AND_ROOT ).at( directive.location, directive.length ) );
    expectLineEnd();
    return;
  }
  // `.own` may name who follows the address, one name or a list; `.root`
  // hands it to the hardware, which has no name.
  std::vector<Token> followers;
  while ( mCursor->at( TokenKind::IDENTIFIER ) )
  {
    followers.push_back( mCursor->advance() );
    if ( !mCursor->match( TokenKind::COMMA ) )
    {
      break;
    }
    if ( !mCursor->at( TokenKind::IDENTIFIER ) )
    {
      Token const here = mCursor->current();
      report(
          diag::diagnostic( diag::DiagnosticId::EXPECTED_NAME ).at( here.location, here.length ).arg( "after", "," ) );
      recover();
      return;
    }
  }
  if ( kind == model::Taking::ROOT && !followers.empty() )
  {
    report( diag::diagnostic( diag::DiagnosticId::ROOT_TAKES_NO_NAME ).at( directive.location, directive.length ) );
    followers.clear();
  }
  mBuilder->taking( kind, std::move( followers ), directive.span() );
  mPendingTaking = kind;
  mTakingToken = directive;
  expectLineEnd();
}

void Parser::parseDeclare( Token directive )
{
  // A `.declare` says what a byte of a Proc is, so there has to be a Proc
  // for it to be a byte of. Outside one the line means nothing at all,
  // which is why the Builder is told nothing.
  if ( !mOpenProc.has_value() )
  {
    report( diag::diagnostic( diag::DiagnosticId::DECLARE_OUTSIDE_PROC ).at( directive.location, directive.length ) );
    recover();
    return;
  }
  if ( refusedOutsideBody( directive, "`.declare`" ) )
  {
    return;
  }

  Token const kindToken = mCursor->current();
  std::string_view const kindWord = kindToken.kind == TokenKind::IDENTIFIER ? textOf( kindToken ) : std::string_view{};
  if ( kindWord != "arg" && kindWord != "ret" )
  {
    report( diag::diagnostic( diag::DiagnosticId::DECLARE_NEEDS_KIND )
                .at( kindToken.location, kindToken.length )
                .arg( "token", std::string{ textOf( kindToken ) } ) );
    recover();
    return;
  }
  mCursor->advance();

  // The type is optional: where none is written the shape of the
  // reservation answers, and where one is, the two are held to each other —
  // see docs/decisions/0081-a-procs-signature-is-declared.md. Or a place in
  // its stead, a letter for each byte from the low one: `a`, `x` or `y` for a
  // register, `m` for a byte of the reservation below — see
  // docs/decisions/0145-an-argument-in-a-register.md.
  std::optional<model::DeclaredType> type;
  std::string place;
  if ( mCursor->at( TokenKind::IDENTIFIER ) )
  {
    std::string_view const word = textOf( mCursor->current() );
    if ( isPlace( word ) )
    {
      Token const written = mCursor->advance();
      if ( word.size() == 2 && word[0] == word[1] )
      {
        report( diag::diagnostic( diag::DiagnosticId::DECLARE_PLACE_REPEATS )
                    .at( written.location, written.length )
                    .arg( "place", std::string{ word } ) );
        recover();
        return;
      }
      place = std::string{ word };
    }
    else
    {
      type = parseDeclaredType();
      if ( !type.has_value() )
      {
        recover();
        return;
      }
    }
  }

  bool const inRegisters = !place.empty() && !place.contains( 'm' );
  mBuilder->declare(
      kindWord == "arg" ? model::Declaring::ARGUMENT : model::Declaring::RESULT, type, place, directive.span() );
  if ( inRegisters )
  {
    mDeclaredInRegisters = true;
    mRegisterToken = directive;
  }
  else
  {
    mDeclarePending = true;
    mDeclareToken = directive;
  }
  expectLineEnd();
}

bool Parser::isPlace( std::string_view word )
{
  // One or two bytes, each a register or memory, and at least one of them a
  // register: `m` and `mm` are the reservation alone, which is what writing
  // no place already says.
  bool const letters =
      !word.empty() && word.size() <= 2 &&
      std::ranges::all_of( word, []( char c ) { return c == 'a' || c == 'x' || c == 'y' || c == 'm'; } );
  return letters && std::ranges::any_of( word, []( char c ) { return c != 'm'; } );
}

std::optional<model::DeclaredType> Parser::parseDeclaredType()
{
  Token const name = mCursor->advance();
  std::string_view const word = textOf( name );
  std::optional<model::DeclaredType::Kind> kind;
  if ( word == "u8" )
  {
    kind = model::DeclaredType::Kind::U8;
  }
  else if ( word == "i8" )
  {
    kind = model::DeclaredType::Kind::I8;
  }
  else if ( word == "u16" )
  {
    kind = model::DeclaredType::Kind::U16;
  }
  else if ( word == "i16" )
  {
    kind = model::DeclaredType::Kind::I16;
  }
  else if ( word == "bool" )
  {
    kind = model::DeclaredType::Kind::BOOL;
  }

  if ( !kind.has_value() )
  {
    report( diag::diagnostic( diag::DiagnosticId::DECLARE_UNKNOWN_TYPE )
                .at( name.location, name.length )
                .arg( "name", std::string{ word } ) );
    return std::nullopt;
  }
  if ( !mCursor->at( TokenKind::LEFT_BRACKET ) )
  {
    return model::DeclaredType{ .kind = *kind, .count = 1, .span = name.span() };
  }

  // `u8[N]` is the one type with a count, and the count is a literal: the
  // compiler writes the type and knows the number, and an author who would
  // rather name it writes no type at all and lets the reservation say it.
  mCursor->advance(); // `[`
  Token const count = mCursor->current();
  std::optional<std::int64_t> const value =
      count.kind == TokenKind::NUMBER ? numericValueOf( textOf( count ) ) : std::nullopt;
  if ( *kind != model::DeclaredType::Kind::U8 || !value.has_value() || *value < 1 || *value > 0xFFFF )
  {
    report( diag::diagnostic( diag::DiagnosticId::DECLARE_UNKNOWN_TYPE )
                .at( name.location, name.length )
                .arg( "name", std::string{ word } + "[" + std::string{ textOf( count ) } + "]" ) );
    return std::nullopt;
  }
  mCursor->advance();
  if ( !mCursor->at( TokenKind::RIGHT_BRACKET ) )
  {
    report( diag::diagnostic( diag::DiagnosticId::DECLARE_UNKNOWN_TYPE )
                .at( name.location, name.length )
                .arg( "name", std::string{ word } + "[" + std::string{ textOf( count ) } ) );
    return std::nullopt;
  }
  Token const close = mCursor->advance();
  return model::DeclaredType{ .kind = model::DeclaredType::Kind::BYTES,
                              .count = static_cast<std::uint32_t>( *value ),
                              .span = spanning( name.span(), close.span() ) };
}

void Parser::parseWith( Token directive )
{
  // `.with NAME`, `.with NAME, x` or `.with NAME = STATE`: what is named is
  // the Builder's to tell apart, and `x` is recognised by position, as it
  // is inside an operand.
  ExpressionPtr what = parseExpression();
  if ( containsError( *what ) )
  {
    recover();
    return;
  }
  model::WithForm form = model::WithForm::SHOW;
  std::optional<Token> state;
  diag::SourceSpan span = spanning( directive.span(), what->span );
  if ( Token const comma = mCursor->current(); mCursor->match( TokenKind::COMMA ) )
  {
    if ( !( mCursor->at( TokenKind::IDENTIFIER ) && textOf( mCursor->current() ) == "x" ) )
    {
      report( diag::diagnostic( diag::DiagnosticId::EXPECTED_NAME )
                  .at( comma.location, comma.length )
                  .arg( "after", std::string{ textOf( comma ) } ) );
      recover();
      return;
    }
    Token const x = mCursor->advance();
    form = model::WithForm::AT;
    span = spanning( directive.span(), x.span() );
  }
  else if ( Token const equals = mCursor->current(); mCursor->match( TokenKind::EQUAL ) )
  {
    if ( !mCursor->at( TokenKind::IDENTIFIER ) )
    {
      report( diag::diagnostic( diag::DiagnosticId::EXPECTED_NAME )
                  .at( equals.location, equals.length )
                  .arg( "after", std::string{ textOf( equals ) } ) );
      recover();
      return;
    }
    state = mCursor->advance();
    form = model::WithForm::STATE;
    span = spanning( directive.span(), state->span() );
  }
  mBuilder->with( form, std::move( what ), state, span );
  mWithPending = true;
  mWithToken = directive;
  expectLineEnd();
}

void Parser::flushPendingOffs( std::optional<diag::SourceLocation> statement )
{
  for ( PendingOff const& pending : mPendingOffs )
  {
    mBuilder->silence( pending.code, statement, pending.span );
  }
  mPendingOffs.clear();
}

void Parser::parseOff( Token directive )
{
  do
  {
    if ( !mCursor->at( TokenKind::IDENTIFIER ) )
    {
      report( diag::diagnostic( diag::DiagnosticId::EXPECTED_NAME )
                  .at( directive.location, directive.length )
                  .arg( "after", std::string{ textOf( directive ) } ) );
      recover();
      return;
    }
    Token const code = mCursor->advance();
    mPendingOffs.push_back( PendingOff{ .code = code, .span = spanning( directive.span(), code.span() ) } );
  } while ( mCursor->match( TokenKind::COMMA ) );

  expectLineEnd();
}

void Parser::parseSource( Token directive )
{
  // A bare string, as a Charset entry or the Project's module path is
  // written: the path names a file, not text for the target, so no prefix.
  auto const refuse = [&]( Token const& found )
  {
    report( diag::diagnostic( diag::DiagnosticId::SOURCE_TAKES_PATH_AND_LINE )
                .at( found.location, found.length )
                .arg( "token", std::string{ textOf( found ) } ) );
    recover();
  };
  if ( !mCursor->at( TokenKind::STRING ) || !quotedOf( textOf( mCursor->current() ) ).charset.empty() )
  {
    refuse( mCursor->current() );
    return;
  }
  Token const path = mCursor->advance();
  if ( !mCursor->match( TokenKind::COMMA ) || !mCursor->at( TokenKind::NUMBER ) )
  {
    refuse( mCursor->current() );
    return;
  }
  Token const line = mCursor->advance();
  mBuilder->markSource( path, line, spanning( directive.span(), line.span() ) );
  expectLineEnd();
}

void Parser::parseLine()
{
  if ( mCursor->atLineEnd() )
  {
    mCursor->match( TokenKind::LINE_END );
    return;
  }

  // Whatever this line holds is the statement a pending `.off` applies to,
  // unless it is another `.off`, which joins the queue. A pending `.with`
  // applies to the same line, if it is an instruction or a macro use, and
  // is refused by whatever else it is; another `.with` or an `.off` joins.
  if ( !atOffDirective() )
  {
    flushPendingOffs( mCursor->current().location );
  }
  mWithsHanded = false;
  mTakingHanded = false;
  if ( mWithPending && !atOffDirective() && !atWithDirective() && !atTakingDirective() )
  {
    Token const first = mCursor->current();
    bool const code = startsLabelDefinition( first ) ? mCursor->peek( 1 ).kind == TokenKind::IDENTIFIER
                                                     : first.kind == TokenKind::IDENTIFIER;
    if ( !code )
    {
      report( diag::diagnostic( diag::DiagnosticId::WITH_NEEDS_CODE ).at( mWithToken.location, mWithToken.length ) );
      mBuilder->dropWiths();
    }
    mWithsHanded = code;
    mWithPending = false;
  }
  // A pending `.own` or `.root` applies to the same line, if it is an
  // instruction or data; another of them, a `.with` or an `.off` joins.
  if ( mPendingTaking.has_value() && !atOffDirective() && !atWithDirective() && !atTakingDirective() )
  {
    mTakingHanded = atInstructionOrData();
    if ( !mTakingHanded )
    {
      report( diag::diagnostic( diag::DiagnosticId::TAKING_NEEDS_STATEMENT )
                  .at( mTakingToken.location, mTakingToken.length )
                  .arg( "directive", std::string{ textOf( mTakingToken ) } ) );
      mBuilder->dropTaking();
    }
    mPendingTaking.reset();
  }
  // A pending `.declare` applies to the `.ztemp` or `.temp` below it and to
  // nothing else, so an instruction standing there is a mistake — but a
  // second `.declare` is not: both then say what the one reservation below
  // them is, which is how a Proc says that the byte it takes an argument in
  // is the byte it leaves its result in. See
  // docs/decisions/0119-one-temporary-carries-two-roles.md.
  // A `.declare` whose bytes are all in registers takes no reservation, so one
  // standing right below it — with no other `.declare` waiting for it — is a
  // mistake: see docs/decisions/0145-an-argument-in-a-register.md.
  if ( mDeclaredInRegisters && !atDeclareDirective() )
  {
    if ( atTemporaryStatement() && !mDeclarePending )
    {
      report( diag::diagnostic( diag::DiagnosticId::DECLARE_REGISTER_TAKES_NO_TEMPORARY )
                  .at( mRegisterToken.location, mRegisterToken.length ) );
    }
    mDeclaredInRegisters = false;
  }
  if ( mDeclarePending && !atDeclareDirective() )
  {
    if ( !atTemporaryStatement() )
    {
      report( diag::diagnostic( diag::DiagnosticId::DECLARE_NEEDS_TEMPORARY )
                  .at( mDeclareToken.location, mDeclareToken.length ) );
      mBuilder->dropDeclaration();
    }
    mDeclarePending = false;
  }

  std::optional<Token> label;
  if ( startsLabelDefinition( mCursor->current() ) )
  {
    // One token of lookahead is the whole of what separates the two things an
    // identifier at column one can begin.
    if ( mCursor->at( TokenKind::IDENTIFIER ) && mCursor->peek( 1 ).kind == TokenKind::EQUAL )
    {
      if ( mOpenMacro.has_value() )
      {
        // A Constant is a Symbol of the Module, and a body defines none: it
        // would be defined once per expansion, or once for a body used by
        // nothing.
        Token const here = mCursor->current();
        report( diag::diagnostic( diag::DiagnosticId::NOT_IN_MACRO_BODY )
                    .at( here.location, here.length )
                    .arg( "what", "a constant definition" ) );
        recover();
        return;
      }
      if ( !mOpenConditionals.empty() )
      {
        Token const here = mCursor->current();
        report( diag::diagnostic( diag::DiagnosticId::NOT_IN_CONDITIONAL )
                    .at( here.location, here.length )
                    .arg( "what", "a constant definition" ) );
        recover();
        return;
      }
      parseConstantDefinition();
      return;
    }
    if ( atTemporaryStatement() )
    {
      std::string const what = "`." + std::string{ textOf( mCursor->peek( 2 ) ) } + "`";
      if ( mOpenMacro.has_value() )
      {
        Token const here = mCursor->peek( 1 );
        report( diag::diagnostic( diag::DiagnosticId::NOT_IN_MACRO_BODY )
                    .at( here.location, here.length + mCursor->peek( 2 ).length )
                    .arg( "what", what ) );
        recover();
        return;
      }
      if ( !mOpenConditionals.empty() )
      {
        Token const here = mCursor->peek( 1 );
        report( diag::diagnostic( diag::DiagnosticId::NOT_IN_CONDITIONAL )
                    .at( here.location, here.length + mCursor->peek( 2 ).length )
                    .arg( "what", what ) );
        recover();
        return;
      }
      parseTemporary();
      return;
    }
    if ( !statementAllowed() )
    {
      return;
    }
    // A local label outside a `.proc` has a finding of its own, which says
    // more than this one would.
    Token const first = mCursor->current();
    bool const local = first.kind == TokenKind::LOCAL_IDENTIFIER || first.kind == TokenKind::ANONYMOUS_LABEL;
    if ( ( !local || inProcNamespace() ) && refusedOutsideBody( first, textOf( first ) ) )
    {
      return;
    }
    label = parseLabelDefinition();
  }

  if ( mCursor->atLineEnd() )
  {
    mCursor->match( TokenKind::LINE_END );
    return;
  }

  parseStatement( label );
}

std::optional<Token> Parser::parseLabelDefinition()
{
  Token const name = mCursor->advance();

  if ( name.direction != Direction::NONE )
  {
    report( diag::diagnostic( diag::DiagnosticId::DIRECTION_IN_DEFINITION ).at( name.location, name.length ) );
    return name;
  }

  bool const isLocal = name.kind == TokenKind::LOCAL_IDENTIFIER || name.kind == TokenKind::ANONYMOUS_LABEL;
  if ( isLocal && !mOpenProc.has_value() && !mOpenMacro.has_value() )
  {
    report( diag::diagnostic( diag::DiagnosticId::LOCAL_LABEL_OUTSIDE_PROC ).at( name.location, name.length ) );
    return name;
  }
  if ( !isLocal && !mOpenConditionals.empty() )
  {
    // A branch defines no Symbol, and the two branches of one `.if` want one
    // name: only a local label may repeat and be taken by direction.
    report( diag::diagnostic( diag::DiagnosticId::LABEL_IN_CONDITIONAL ).at( name.location, name.length ) );
    return name;
  }
  mBuilder->defineLabel( name, name.span() );
  return name;
}

void Parser::parseConstantDefinition()
{
  Token const name = mCursor->advance();
  mCursor->advance(); // `=`

  ExpressionPtr value = parseExpression();
  bool const quiet = containsError( *value );
  diag::SourceSpan const span = spanning( name.span(), value->span );
  mBuilder->defineConstant( name, std::move( value ), span );
  endStatement( quiet );
}

void Parser::parseStatement( std::optional<Token> label )
{
  switch ( mCursor->kind() )
  {
  case TokenKind::DOT:
    parseDirective();
    return;
  case TokenKind::IDENTIFIER:
    if ( !statementAllowed() )
    {
      return;
    }
    if ( refusedOutsideBody( mCursor->current(), textOf( mCursor->current() ) ) )
    {
      return;
    }
    if ( atQualifiedUse() )
    {
      parseQualifiedUse();
      return;
    }
    parseInstruction();
    return;
  case TokenKind::UNKNOWN:
    // Already diagnosed by the lexer. Adding a parse error on top is what turns
    // one bad character into a column of findings.
    recover();
    return;
  default:
    break;
  }

  Token const here = mCursor->current();
  if ( label.has_value() )
  {
    // The name was almost certainly meant to be an instruction, and whatever
    // follows it is the symptom. Report the indent, against the name.
    report( diag::diagnostic( diag::DiagnosticId::INDENT_EXPECTED )
                .at( label->location, label->length )
                .arg( "name", describe( *label ) ) );
  }
  else
  {
    report( diag::diagnostic( diag::DiagnosticId::STATEMENT_EXPECTED )
                .at( here.location, here.length )
                .arg( "token", describe( here ) ) );
  }
  recover();
}

void Parser::parseDirective()
{
  Token const dot = mCursor->advance();
  if ( !mCursor->at( TokenKind::IDENTIFIER ) )
  {
    report( diag::diagnostic( diag::DiagnosticId::EXPECTED_NAME ).at( dot.location, dot.length ).arg( "after", "." ) );
    recover();
    return;
  }

  Token const name = mCursor->advance();
  Token const directive{ .kind = TokenKind::DOT,
                         .startsLine = dot.startsLine,
                         .direction = Direction::NONE,
                         .location = dot.location,
                         .length = dot.length + name.length };
  std::string_view const spelling = textOf( directive );
  std::string_view const word = textOf( name );

  // A body holds instructions and data: the directives that emit data pass,
  // the ones that close the body pass, and `.macro` is refused with a word of
  // its own below. Everything else declares, reserves or scopes, and a body
  // does none of that — except `.source`, a mark by position that does
  // neither and covers every use of the body, and `.with`, which shows a
  // Pane over one statement of the body and puts back, at each use, what
  // the use's Section shows — see docs/decisions/0096-panes-in-c.md.
  if ( mOpenMacro.has_value() && !isDataWord( word ) && word != "endm" && word != "endmacro" && word != "macro" &&
       word != "source" && word != "with" && !isConditionalWord( word ) )
  {
    report( diag::diagnostic( diag::DiagnosticId::NOT_IN_MACRO_BODY )
                .at( directive.location, directive.length )
                .arg( "what", "`" + std::string{ spelling } + "`" ) );
    recover();
    return;
  }

  // A branch holds instructions and data, as a macro body does, and for the
  // same reason turned around: nothing in it may define a Symbol, since two
  // branches want one name and only one of them will be there.
  // `.source` is a mark by position and defines nothing, so it stands in
  // a branch as it stands anywhere.
  if ( !mOpenConditionals.empty() && !isDataWord( word ) && word != "res" && word != "source" && word != "with" &&
       !isConditionalWord( word ) )
  {
    report( diag::diagnostic( diag::DiagnosticId::NOT_IN_CONDITIONAL )
                .at( directive.location, directive.length )
                .arg( "what", "`" + std::string{ spelling } + "`" ) );
    recover();
    return;
  }

  if ( word != "case" && word != "endmatch" && !statementAllowed() )
  {
    return;
  }

  // What emits, reserves or opens a branch that would, stands in a Section.
  // A `.if` refused here still opens, so that its branches and its end pair
  // with it and what it holds is refused without a word each.
  if ( ( isDataWord( word ) || word == "res" || word == "transition" || word == "dispatch" || word == "if" ) &&
       refusedOutsideBody( directive, spelling ) )
  {
    if ( word == "if" )
    {
      mOpenConditionals.push_back( OpenConditional{ .directive = directive, .outside = true } );
    }
    return;
  }

  if ( word == "if" )
  {
    parseConditional( directive );
  }
  else if ( word == "elsif" || word == "else" )
  {
    parseBranch( directive, word == "else" );
  }
  else if ( word == "endif" || word == "endi" )
  {
    parseConditionalEnd( directive, spelling );
  }
  else if ( word == "match" )
  {
    parseMatch( directive );
  }
  else if ( word == "case" )
  {
    parseCase( directive );
  }
  else if ( word == "endmatch" )
  {
    parseMatchEnd( directive, spelling );
  }
  else if ( word == "section" )
  {
    parseSection( directive );
  }
  else if ( word == "endsection" || word == "ends" )
  {
    parseSectionEnd( directive, spelling );
  }
  else if ( word == "proc" )
  {
    parseProc( directive );
  }
  else if ( word == "endproc" || word == "endp" )
  {
    parseProcEnd( directive, spelling );
  }
  else if ( word == "byte" )
  {
    parseData( directive, DataWidth::BYTE );
  }
  else if ( word == "word" )
  {
    parseData( directive, DataWidth::WORD );
  }
  else if ( word == "hex" )
  {
    parsePayload( directive, Payload::HEX );
  }
  else if ( word == "binary" )
  {
    parsePayload( directive, Payload::BINARY );
  }
  else if ( word == "base64" )
  {
    parsePayload( directive, Payload::BASE64 );
  }
  else if ( word == "res" )
  {
    parseReserve( directive );
  }
  else if ( word == "ztemp" || word == "temp" )
  {
    // A name at column one is read before the directive, so one that was
    // written has already brought us to parseTemporary; here there was none.
    report( diag::diagnostic( diag::DiagnosticId::TEMPORARY_NEEDS_NAME ).at( directive.location, directive.length ) );
    recover();
  }
  else if ( word == "assert" )
  {
    parseAssertion( directive );
  }
  else if ( word == "transition" )
  {
    parseTransition( directive );
  }
  else if ( word == "dispatch" )
  {
    parseDispatch( directive );
  }
  else if ( word == "macro" )
  {
    parseMacro( directive );
  }
  else if ( word == "endmacro" || word == "endm" )
  {
    parseMacroEnd( directive, spelling );
  }
  else if ( word == "namespace" )
  {
    parseNamespace( directive );
  }
  else if ( word == "endnamespace" || word == "endns" )
  {
    parseNamespaceEnd( directive, spelling );
  }
  else if ( word == "export" )
  {
    parseExport( directive );
  }
  else if ( word == "off" )
  {
    parseOff( directive );
  }
  else if ( word == "source" )
  {
    parseSource( directive );
  }
  else if ( word == "with" )
  {
    parseWith( directive );
  }
  else if ( word == "own" )
  {
    parseTaking( directive, model::Taking::OWN );
  }
  else if ( word == "root" )
  {
    parseTaking( directive, model::Taking::ROOT );
  }
  else if ( word == "declare" )
  {
    parseDeclare( directive );
  }
  else if ( word == "transform" )
  {
    parseTwoNames( directive, &Builder::declareTransform );
  }
  else if ( word == "driver" )
  {
    parseDriver( directive );
  }
  else if ( word == "slot" )
  {
    parseSlot( directive );
  }
  else if ( word == "implements" )
  {
    parseImplements( directive );
  }
  else if ( word == "charset" )
  {
    if ( !mOpenNamespaces.empty() )
    {
      // A literal's prefix is one identifier and the lexer never looks past
      // a dot for a quote, so a Charset in a Namespace could not be named
      // from outside it: it stands at the top level, and the queue's entry
      // on `game.font"..."` closes with that.
      report( diag::diagnostic( diag::DiagnosticId::CHARSET_IN_NAMESPACE ).at( directive.location, directive.length ) );
      recover();
      return;
    }
    parseCharset( directive );
  }
  else if ( word == "endcharset" || word == "endch" )
  {
    report( diag::diagnostic( diag::DiagnosticId::UNMATCHED_CHARSET_END )
                .at( directive.location, directive.length )
                .arg( "directive", std::string{ spelling } ) );
    recover();
  }
  else if ( word == "end" )
  {
    report(
        diag::diagnostic( diag::DiagnosticId::AMBIGUOUS_END_DIRECTIVE ).at( directive.location, directive.length ) );
    recover();
  }
  else
  {
    report( diag::diagnostic( diag::DiagnosticId::UNKNOWN_DIRECTIVE )
                .at( directive.location, directive.length )
                .arg( "name", std::string{ word } ) );
    recover();
  }
}

void Parser::parseSection( Token directive )
{
  if ( mOpenSection.has_value() )
  {
    report( diag::diagnostic( diag::DiagnosticId::SECTION_INSIDE_SECTION ).at( directive.location, directive.length ) );
    recover();
    return;
  }

  // A Section has no name of its own: its first Label names it, so the
  // attributes follow the directive at once.
  SectionAttributes attributes;
  bool quiet = false;
  if ( !parseSectionAttributes( attributes, quiet, false ) )
  {
    return;
  }

  // A Temporary is not in a Pane, whose Bank has no room for the pair that
  // shares; not movable, which the queue leaves undecided; and not a Root,
  // which the hardware reaches at any time, so that its bytes are never
  // another's. The other attribute is dropped and the Section stays a
  // Temporary.
  if ( attributes.temporary && attributes.pane.has_value() )
  {
    report( diag::diagnostic( diag::DiagnosticId::TEMPORARY_EXCLUDES )
                .at( directive.location, directive.length )
                .arg( "other", "in" ) );
    attributes.pane.reset();
  }
  // `as` names the type whose Temporaries hold a Proc's arguments; data has
  // no arguments.
  if ( attributes.as.has_value() )
  {
    report( diag::diagnostic( diag::DiagnosticId::AS_ATTRIBUTE )
                .at( attributes.as->location, attributes.as->length )
                .arg( "name", std::string{ textOf( *attributes.as ) } )
                .arg( "reason", "stands on a `.section`, and only a `.proc` takes arguments" ) );
    attributes.as.reset();
  }
  // `under` states what is shown while code runs; data does not run.
  if ( attributes.under.has_value() )
  {
    report( diag::diagnostic( diag::DiagnosticId::UNDER_ATTRIBUTE )
                .at( attributes.under->location, attributes.under->length )
                .arg( "name", std::string{ textOf( *attributes.under ) } )
                .arg( "reason", "stands on a `.section`, and only a `.proc` runs" ) );
    attributes.under.reset();
  }
  if ( attributes.temporary && attributes.movable )
  {
    report( diag::diagnostic( diag::DiagnosticId::TEMPORARY_EXCLUDES )
                .at( directive.location, directive.length )
                .arg( "other", "movable" ) );
    attributes.movable = false;
  }
  if ( attributes.temporary && attributes.root )
  {
    report( diag::diagnostic( diag::DiagnosticId::TEMPORARY_EXCLUDES )
                .at( directive.location, directive.length )
                .arg( "other", "root" ) );
    attributes.root = false;
  }
  // A Temporary holds reservations alone, so it has no bytes for `readonly` to
  // be about; a Movable Section is copied from its Payload at every edge it
  // survives at a different address, which is a write; and the zero page is
  // the scarcest memory the machine has, which a Section nothing writes gains
  // nothing from — see
  // docs/decisions/0215-a-cartridge-is-rom-and-a-section-stands-in-it-when-nothing-writes-it.md.
  if ( attributes.readOnly && attributes.temporary )
  {
    report( diag::diagnostic( diag::DiagnosticId::TEMPORARY_EXCLUDES )
                .at( directive.location, directive.length )
                .arg( "other", "readonly" ) );
    attributes.readOnly = false;
  }
  if ( attributes.readOnly && attributes.movable )
  {
    report( diag::diagnostic( diag::DiagnosticId::READONLY_EXCLUDES )
                .at( directive.location, directive.length )
                .arg( "other", "movable" ) );
    attributes.readOnly = false;
  }
  if ( attributes.readOnly && attributes.placement == model::PlacementClass::ZEROPAGE )
  {
    report( diag::diagnostic( diag::DiagnosticId::READONLY_EXCLUDES )
                .at( directive.location, directive.length )
                .arg( "other", "zeropage" ) );
    attributes.readOnly = false;
  }

  mOpenSection = directive;
  mBuilder->beginSection( std::move( attributes ), directive.span() );
  endStatement( quiet );
}

bool Parser::parseSectionAttributes( SectionAttributes& attributes, bool& quiet, bool leadingComma )
{
  bool placementGiven = false;
  // After a `.proc`'s name every attribute follows a comma; after `.section`
  // the first one follows the directive itself.
  bool first = !leadingComma;
  while ( ( first && mCursor->at( TokenKind::IDENTIFIER ) ) || mCursor->match( TokenKind::COMMA ) )
  {
    first = false;
    if ( !mCursor->at( TokenKind::IDENTIFIER ) )
    {
      Token const here = mCursor->current();
      report( diag::diagnostic( diag::DiagnosticId::UNKNOWN_SECTION_ATTRIBUTE )
                  .at( here.location, here.length )
                  .arg( "name", describe( here ) ) );
      recover();
      return false;
    }

    Token const attribute = mCursor->advance();
    std::string_view const word = textOf( attribute );

    // `movable` is an attribute of its own beside the placement: it says how
    // the Section lives across Phases, not where.
    if ( word == "movable" )
    {
      if ( attributes.movable )
      {
        report( diag::diagnostic( diag::DiagnosticId::REPEATED_SECTION_ATTRIBUTE )
                    .at( attribute.location, attribute.length )
                    .arg( "name", std::string{ word } ) );
      }
      attributes.movable = true;
      continue;
    }

    // `temporary` likewise: how long the bytes are the Section's own.
    if ( word == "temporary" )
    {
      if ( attributes.temporary )
      {
        report( diag::diagnostic( diag::DiagnosticId::REPEATED_SECTION_ATTRIBUTE )
                    .at( attribute.location, attribute.length )
                    .arg( "name", std::string{ word } ) );
      }
      attributes.temporary = true;
      continue;
    }

    // `readonly` likewise: whether the bytes ever change, not where they are.
    if ( word == "readonly" )
    {
      if ( attributes.readOnly )
      {
        report( diag::diagnostic( diag::DiagnosticId::REPEATED_SECTION_ATTRIBUTE )
                    .at( attribute.location, attribute.length )
                    .arg( "name", std::string{ word } ) );
      }
      attributes.readOnly = true;
      continue;
    }

    // `root` likewise: what reaches the Section, not where it stands.
    if ( word == "root" )
    {
      if ( attributes.root )
      {
        report( diag::diagnostic( diag::DiagnosticId::REPEATED_SECTION_ATTRIBUTE )
                    .at( attribute.location, attribute.length )
                    .arg( "name", std::string{ word } ) );
      }
      attributes.root = true;
      continue;
    }

    // `in PANE`: which Pane the Section is in, a declaration about place
    // beside the placement — see docs/decisions/0054-panes.md.
    if ( word == "in" )
    {
      if ( !mCursor->at( TokenKind::IDENTIFIER ) )
      {
        report( diag::diagnostic( diag::DiagnosticId::EXPECTED_NAME )
                    .at( attribute.location, attribute.length )
                    .arg( "after", std::string{ word } ) );
        recover();
        return false;
      }
      if ( attributes.pane.has_value() )
      {
        report( diag::diagnostic( diag::DiagnosticId::REPEATED_SECTION_ATTRIBUTE )
                    .at( attribute.location, attribute.length )
                    .arg( "name", std::string{ word } ) );
      }
      attributes.pane = mCursor->advance();
      continue;
    }

    // `as TYPE`: the Proc's arguments are the Temporaries of the Proc named —
    // see docs/decisions/0065-handlers.md.
    if ( word == "as" )
    {
      if ( !mCursor->at( TokenKind::IDENTIFIER ) )
      {
        report( diag::diagnostic( diag::DiagnosticId::EXPECTED_NAME )
                    .at( attribute.location, attribute.length )
                    .arg( "after", std::string{ word } ) );
        recover();
        return false;
      }
      if ( attributes.as.has_value() )
      {
        report( diag::diagnostic( diag::DiagnosticId::REPEATED_SECTION_ATTRIBUTE )
                    .at( attribute.location, attribute.length )
                    .arg( "name", std::string{ word } ) );
      }
      attributes.as = mCursor->advance();
      continue;
    }

    // `under PANE`: the Proc runs while the Pane is shown — see
    // docs/decisions/0098-a-proc-declares-what-is-shown.md.
    if ( word == "under" )
    {
      if ( !mCursor->at( TokenKind::IDENTIFIER ) )
      {
        report( diag::diagnostic( diag::DiagnosticId::EXPECTED_NAME )
                    .at( attribute.location, attribute.length )
                    .arg( "after", std::string{ word } ) );
        recover();
        return false;
      }
      if ( attributes.under.has_value() )
      {
        report( diag::diagnostic( diag::DiagnosticId::REPEATED_SECTION_ATTRIBUTE )
                    .at( attribute.location, attribute.length )
                    .arg( "name", std::string{ word } ) );
      }
      attributes.under = mCursor->advance();
      continue;
    }

    if ( word != "zeropage" && word != "absolute" )
    {
      // `at` reaches here only when it was written without a placement in front
      // of it, and "not an attribute" is exactly what it then is.
      report( diag::diagnostic( diag::DiagnosticId::UNKNOWN_SECTION_ATTRIBUTE )
                  .at( attribute.location, attribute.length )
                  .arg( "name", std::string{ word } ) );
      recover();
      return false;
    }

    // `at`, `align` and `within` complete the placement rather than being
    // attributes of their own, so each is recognised by position and never
    // stands after a comma. Nothing becomes a keyword by this, exactly as
    // with `x` and `y` in an operand. Any order, each at most once.
    SectionAttributes given;
    given.placement = word == "zeropage" ? model::PlacementClass::ZEROPAGE : model::PlacementClass::ABSOLUTE;
    while ( mCursor->at( TokenKind::IDENTIFIER ) )
    {
      std::string_view const qualifier = textOf( mCursor->current() );
      ExpressionPtr* slot = nullptr;
      if ( qualifier == "at" )
      {
        slot = &given.pinnedAddress;
      }
      else if ( qualifier == "align" )
      {
        slot = &given.alignment;
      }
      else if ( qualifier == "within" )
      {
        slot = &given.boundary;
      }
      else
      {
        break;
      }
      Token const keyword = mCursor->advance();
      ExpressionPtr value = parseExpression();
      quiet = quiet || containsError( *value );
      if ( *slot != nullptr )
      {
        report( diag::diagnostic( diag::DiagnosticId::DUPLICATE_PLACEMENT_QUALIFIER )
                    .at( keyword.location, keyword.length )
                    .arg( "name", std::string{ qualifier } ) );
        continue;
      }
      *slot = std::move( value );
    }

    // A second placement is reported and dropped rather than recovered from:
    // the Section is well formed without it, and skipping the line as well
    // would report its `.ends` as unmatched on top.
    if ( placementGiven )
    {
      report( diag::diagnostic( diag::DiagnosticId::DUPLICATE_SECTION_ATTRIBUTE )
                  .at( attribute.location, attribute.length )
                  .arg( "name", std::string{ word } ) );
      continue;
    }

    placementGiven = true;
    given.movable = attributes.movable;
    given.root = attributes.root;
    given.pane = attributes.pane;
    given.under = attributes.under;
    given.as = attributes.as;
    given.temporary = attributes.temporary;
    attributes = std::move( given );
  }
  return true;
}

void Parser::parseSectionEnd( Token directive, std::string_view spelling )
{
  if ( !mOpenSection.has_value() )
  {
    report( diag::diagnostic( diag::DiagnosticId::UNMATCHED_SECTION_END )
                .at( directive.location, directive.length )
                .arg( "directive", std::string{ spelling } ) );
    recover();
    return;
  }

  mOpenSection.reset();
  mBuilder->endSection( directive.span() );
  expectLineEnd();
}

void Parser::parseProc( Token directive )
{
  if ( mOpenProc.has_value() )
  {
    report( diag::diagnostic( diag::DiagnosticId::PROC_INSIDE_PROC ).at( directive.location, directive.length ) );
    recover();
    return;
  }
  if ( mOpenSection.has_value() )
  {
    report( diag::diagnostic( diag::DiagnosticId::PROC_INSIDE_SECTION ).at( directive.location, directive.length ) );
    recover();
    return;
  }

  if ( !mCursor->at( TokenKind::IDENTIFIER ) )
  {
    report( diag::diagnostic( diag::DiagnosticId::EXPECTED_NAME )
                .at( directive.location, directive.length )
                .arg( "after", std::string{ textOf( directive ) } ) );
    recover();
    return;
  }

  Token const name = mCursor->advance();
  SectionAttributes attributes;
  bool quiet = false;
  if ( !parseSectionAttributes( attributes, quiet ) )
  {
    return;
  }

  // A Proc is code, and code stands wherever a Section may — on the zero
  // page too, where a small routine that patches itself is fastest — see
  // docs/decisions/0059-a-proc-on-the-zero-page.md.
  if ( attributes.temporary )
  {
    report( diag::diagnostic( diag::DiagnosticId::PROC_NOT_TEMPORARY ).at( directive.location, directive.length ) );
    attributes.temporary = false;
  }
  // An address of code taken is a jump and never a write — `.own` says so — so
  // a Proc needs no word to say its bytes do not change.
  if ( attributes.readOnly )
  {
    report( diag::diagnostic( diag::DiagnosticId::PROC_IS_READONLY ).at( directive.location, directive.length ) );
    attributes.readOnly = false;
  }
  // A Proc that declares what is shown stands in `fixed`: in the Pane it names
  // it would be the Pane's own code, and would vanish with a switch it makes.
  if ( attributes.under.has_value() && attributes.pane.has_value() )
  {
    report( diag::diagnostic( diag::DiagnosticId::UNDER_ATTRIBUTE )
                .at( attributes.under->location, attributes.under->length )
                .arg( "name", std::string{ textOf( *attributes.under ) } )
                .arg( "reason", "stands beside `in`, and a proc under a pane is in none" ) );
    attributes.under.reset();
  }

  mOpenProc = directive;
  mNamespacesAtProc = mOpenNamespaces.size();
  mBuilder->beginProc( name, std::move( attributes ), spanning( directive.span(), name.span() ) );
  endStatement( quiet );
}

void Parser::parseMacro( Token directive )
{
  if ( mOpenSection.has_value() || mOpenProc.has_value() || mOpenMacro.has_value() )
  {
    report( diag::diagnostic( diag::DiagnosticId::MACRO_NOT_AT_TOP_LEVEL ).at( directive.location, directive.length ) );
    recover();
    return;
  }
  if ( !mCursor->at( TokenKind::IDENTIFIER ) )
  {
    report( diag::diagnostic( diag::DiagnosticId::EXPECTED_NAME )
                .at( directive.location, directive.length )
                .arg( "after", std::string{ textOf( directive ) } ) );
    recover();
    return;
  }
  Token const name = mCursor->advance();

  // Parameters follow the name, comma-separated, and the list ends where the
  // commas stop; a name written twice is the implementation's to refuse.
  Pattern parameters;
  if ( !parsePattern( parameters, name ) )
  {
    return;
  }

  mOpenMacro = directive;
  mBuilder->beginMacro( name, std::move( parameters ), spanning( directive.span(), name.span() ) );
  expectLineEnd();
}

void Parser::parseMacroEnd( Token directive, std::string_view spelling )
{
  if ( !mOpenMacro.has_value() )
  {
    report( diag::diagnostic( diag::DiagnosticId::UNMATCHED_MACRO_END )
                .at( directive.location, directive.length )
                .arg( "directive", std::string{ spelling } ) );
    recover();
    return;
  }
  mOpenMacro.reset();
  mBuilder->endMacro( directive.span() );
  expectLineEnd();
}

void Parser::parseProcEnd( Token directive, std::string_view spelling )
{
  if ( !mOpenProc.has_value() )
  {
    report( diag::diagnostic( diag::DiagnosticId::UNMATCHED_PROC_END )
                .at( directive.location, directive.length )
                .arg( "directive", std::string{ spelling } ) );
    recover();
    return;
  }

  // A Namespace opened in the Proc closes before the Proc does, and a Section
  // too.
  if ( mOpenSection.has_value() && mOpenNamespaces.size() > mNamespacesAtProc )
  {
    report(
        diag::diagnostic( diag::DiagnosticId::SECTION_NOT_CLOSED ).at( mOpenSection->location, mOpenSection->length ) );
    mOpenSection.reset();
    mBuilder->endSection( directive.span() );
  }
  while ( mOpenNamespaces.size() > mNamespacesAtProc )
  {
    report( diag::diagnostic( diag::DiagnosticId::NAMESPACE_NOT_CLOSED )
                .at( mOpenNamespaces.back().location, mOpenNamespaces.back().length ) );
    mOpenNamespaces.pop_back();
    mBuilder->endNamespace( directive.span() );
  }
  if ( mOpenSection.has_value() )
  {
    report(
        diag::diagnostic( diag::DiagnosticId::SECTION_NOT_CLOSED ).at( mOpenSection->location, mOpenSection->length ) );
    mOpenSection.reset();
    mBuilder->endSection( directive.span() );
  }
  mOpenProc.reset();

  // `then NAME` is recognised by position, as `at` is after a placement, so
  // nothing becomes a keyword by it.
  std::optional<Token> then;
  if ( mCursor->at( TokenKind::IDENTIFIER ) && textOf( mCursor->current() ) == "then" )
  {
    Token const keyword = mCursor->advance();
    if ( !mCursor->at( TokenKind::IDENTIFIER ) )
    {
      report( diag::diagnostic( diag::DiagnosticId::EXPECTED_NAME )
                  .at( keyword.location, keyword.length )
                  .arg( "after", "then" ) );
      mBuilder->endProc( std::nullopt, directive.span() );
      recover();
      return;
    }
    then = mCursor->advance();
  }
  mBuilder->endProc( then, directive.span() );
  expectLineEnd();
}

void Parser::parseData( Token directive, DataWidth width )
{
  std::vector<ExpressionPtr> items;
  diag::SourceSpan span = directive.span();
  bool quiet = false;

  do
  {
    ExpressionPtr item = parseItem();
    quiet = quiet || containsError( *item );
    span = spanning( span, item->span );
    items.push_back( std::move( item ) );
  } while ( mCursor->match( TokenKind::COMMA ) );

  mBuilder->emitData( width, std::move( items ), span );
  endStatement( quiet );
}

/// `.dispatch TARGET [, TARGET]...`: an item list, read as `.byte`'s is. What
/// a target may be is a question about Symbols and belongs to the builder.
void Parser::parseDispatch( Token directive )
{
  std::vector<ExpressionPtr> targets;
  diag::SourceSpan span = directive.span();
  bool quiet = false;

  do
  {
    ExpressionPtr target = parseItem();
    quiet = quiet || containsError( *target );
    span = spanning( span, target->span );
    targets.push_back( std::move( target ) );
  } while ( mCursor->match( TokenKind::COMMA ) );

  mBuilder->dispatch( std::move( targets ), span );
  endStatement( quiet );
}

void Parser::parsePayload( Token directive, Payload encoding )
{
  std::string const spelling{ nameOf( encoding ) };

  if ( !mCursor->at( TokenKind::STRING ) )
  {
    report( diag::diagnostic( diag::DiagnosticId::EXPECTED_PAYLOAD )
                .at( directive.location, directive.length )
                .arg( "encoding", spelling )
                .arg( "token", describe( mCursor->current() ) ) );
    recover();
    return;
  }

  Token const literal = mCursor->advance();
  std::string_view const text = textOf( literal );
  Quoted const quoted = quotedOf( text );

  if ( !quoted.charset.empty() )
  {
    report( diag::diagnostic( diag::DiagnosticId::PAYLOAD_NAMES_CHARSET ).at( literal.location, literal.length ) );
    recover();
    return;
  }

  DecodedPayload const decoded = decodePayload( encoding, quoted.body );

  // Where the body begins inside the token, so that a fault underlines the
  // character the author wrote and not the literal that holds it.
  auto const bodyAt = static_cast<std::uint32_t>( quoted.body.data() - text.data() );

  for ( PayloadFault const& fault : decoded.faults )
  {
    diag::SourceLocation const at =
        diag::SourceLocation::fromRawOffset( literal.location.rawOffset() + bodyAt + fault.offset );

    if ( fault.padding )
    {
      report( diag::diagnostic( diag::DiagnosticId::PAYLOAD_PADDING ).at( at, fault.length ) );
    }
    else
    {
      report( diag::diagnostic( diag::DiagnosticId::PAYLOAD_CHARACTER )
                  .at( at, fault.length )
                  .arg( "character", fault.character )
                  .arg( "encoding", spelling ) );
    }
  }

  // A character the alphabet refused is why the count is short, so the length is
  // not a second thing to say about the same payload.
  if ( decoded.faults.empty() && !decoded.wholeBytes )
  {
    report( diag::diagnostic( diag::DiagnosticId::PAYLOAD_NOT_WHOLE_BYTES )
                .at( literal.location, literal.length )
                .arg( "encoding", spelling )
                .arg( "group", static_cast<std::int64_t>( charactersPerGroup( encoding ) ) )
                .arg( "count", static_cast<std::int64_t>( decoded.characters ) ) );
  }

  if ( !decoded.faults.empty() || !decoded.wholeBytes )
  {
    // Emitting what decoded would put a Chunk of the wrong length in a Section
    // whose every address follows from it. An error stops the pipeline at the
    // end of Assemble, so nothing downstream misses the bytes.
    recover();
    return;
  }

  diag::SourceSpan const span = spanning( directive.span(), literal.span() );

  // A payload is the Chunk `.byte` makes, with its items already folded: the
  // one node that carries a value rather than text — see Expression.hpp.
  std::vector<ExpressionPtr> items;
  items.reserve( decoded.bytes.size() );
  for ( std::uint8_t const byte : decoded.bytes )
  {
    ExpressionPtr item = makeExpression( ExpressionKind::VALUE, literal, span );
    item->value = byte;
    items.push_back( std::move( item ) );
  }

  mBuilder->emitData( DataWidth::BYTE, std::move( items ), span );
  expectLineEnd();
}

void Parser::parseReserve( Token directive )
{
  ExpressionPtr size = parseExpression();
  bool const quiet = containsError( *size );
  diag::SourceSpan const span = spanning( directive.span(), size->span );
  mBuilder->reserve( std::move( size ), span );
  endStatement( quiet );
}

void Parser::parseTemporary()
{
  Token const name = mCursor->advance();
  mCursor->advance(); // `.`
  Token const directive = mCursor->advance();

  if ( name.kind != TokenKind::IDENTIFIER || name.direction != Direction::NONE )
  {
    // A local label is a position inside a Proc, and a Temporary is a
    // Section of its own; the name has to be one the whole Module can use.
    report( diag::diagnostic( diag::DiagnosticId::TEMPORARY_NEEDS_NAME ).at( directive.location, directive.length ) );
    recover();
    return;
  }

  // `.temp` is `.ztemp` off the zero page — see
  // docs/decisions/0085-a-section-may-stand-in-a-proc.md.
  model::PlacementClass const placement =
      textOf( directive ) == "temp" ? model::PlacementClass::ABSOLUTE : model::PlacementClass::ZEROPAGE;
  ExpressionPtr size = parseExpression();
  bool const quiet = containsError( *size );
  diag::SourceSpan const span = spanning( name.span(), size->span );
  mBuilder->reserveTemporary( name, std::move( size ), placement, span );
  endStatement( quiet );
}

void Parser::parseAssertion( Token directive )
{
  ExpressionPtr condition = parseExpression();
  bool const quiet = containsError( *condition );
  diag::SourceSpan const span = spanning( directive.span(), condition->span );
  mBuilder->addAssertion( std::move( condition ), span );
  endStatement( quiet );
}

void Parser::parseExport( Token directive )
{
  do
  {
    if ( !mCursor->at( TokenKind::IDENTIFIER ) )
    {
      report( diag::diagnostic( diag::DiagnosticId::EXPECTED_NAME )
                  .at( directive.location, directive.length )
                  .arg( "after", std::string{ textOf( directive ) } ) );
      recover();
      return;
    }

    Token const name = mCursor->advance();
    mBuilder->exportSymbol( name, name.span() );
  } while ( mCursor->match( TokenKind::COMMA ) );

  expectLineEnd();
}

bool Parser::atCharsetEnd() const
{
  if ( !mCursor->at( TokenKind::DOT ) || mCursor->peek( 1 ).kind != TokenKind::IDENTIFIER )
  {
    return false;
  }
  std::string_view const word = textOf( mCursor->peek( 1 ) );
  return word == "endcharset" || word == "endch";
}

void Parser::parseCharset( Token directive )
{
  std::optional<Token> name;
  std::optional<model::CharsetBase> base;
  bool quiet = false;

  if ( mCursor->at( TokenKind::IDENTIFIER ) )
  {
    name = mCursor->advance();
  }
  else
  {
    report( diag::diagnostic( diag::DiagnosticId::EXPECTED_NAME )
                .at( directive.location, directive.length )
                .arg( "after", std::string{ textOf( directive ) } ) );
    quiet = true;
  }

  if ( name.has_value() && mCursor->match( TokenKind::COLON ) )
  {
    if ( mCursor->at( TokenKind::IDENTIFIER ) )
    {
      Token const baseName = mCursor->advance();
      ExpressionPtr mask;
      if ( mCursor->match( TokenKind::CARET ) )
      {
        mask = parseExpression();
        quiet = quiet || containsError( *mask );
      }
      base = model::CharsetBase{ .name = baseName, .mask = std::move( mask ) };
    }
    else
    {
      Token const here = mCursor->current();
      report(
          diag::diagnostic( diag::DiagnosticId::EXPECTED_NAME ).at( here.location, here.length ).arg( "after", ":" ) );
      quiet = true;
    }
  }

  endStatement( quiet );

  // The body is read here rather than line by line, because it holds entries
  // and not statements. Nothing about the block is therefore open across a
  // line, and a `.section` cannot begin inside one without the block having
  // failed to close first.
  std::vector<model::CharsetEntry> entries;
  bool closed = false;
  while ( !mCursor->atEnd() )
  {
    if ( mCursor->atLineEnd() )
    {
      mCursor->match( TokenKind::LINE_END );
      continue;
    }
    if ( atCharsetEnd() )
    {
      mCursor->advance(); // `.`
      mCursor->advance(); // the word
      expectLineEnd();
      closed = true;
      break;
    }
    if ( mCursor->at( TokenKind::DOT ) )
    {
      // Some other directive. The block was never closed, and that is the whole
      // of what went wrong; the directive is left where it stands so the line
      // loop reads it as the statement it is.
      break;
    }
    parseCharsetEntry( entries );
  }

  if ( !closed )
  {
    report( diag::diagnostic( diag::DiagnosticId::CHARSET_NOT_CLOSED ).at( directive.location, directive.length ) );
  }

  if ( name.has_value() )
  {
    mBuilder->declareCharset( *name, std::move( base ), std::move( entries ), directive.span() );
  }
}

void Parser::parseCharsetEntry( std::vector<model::CharsetEntry>& into )
{
  Token const here = mCursor->current();
  if ( here.kind != TokenKind::STRING )
  {
    report( diag::diagnostic( diag::DiagnosticId::EXPECTED_CHARSET_ENTRY ).at( here.location, here.length ) );
    recover();
    return;
  }

  Token const characters = mCursor->advance();
  Quoted const quoted = quotedOf( textOf( characters ) );
  bool reported = false;
  if ( !quoted.charset.empty() )
  {
    report( diag::diagnostic( diag::DiagnosticId::CHARSET_ENTRY_PREFIXED )
                .at( characters.location, static_cast<std::uint32_t>( quoted.charset.size() ) ) );
    reported = true;
  }
  else if ( characterCountOf( quoted.body ) == 0 )
  {
    report( diag::diagnostic( diag::DiagnosticId::EMPTY_CHARSET_ENTRY ).at( characters.location, characters.length ) );
    reported = true;
  }

  if ( !mCursor->match( TokenKind::EQUAL ) )
  {
    Token const at = mCursor->current();
    report( diag::diagnostic( diag::DiagnosticId::EXPECTED_CHARSET_ENTRY ).at( at.location, at.length ) );
    recover();
    return;
  }

  ExpressionPtr start = parseExpression();
  bool const quiet = containsError( *start );
  if ( !reported )
  {
    into.push_back( model::CharsetEntry{ .characters = characters, .start = std::move( start ) } );
  }
  endStatement( quiet );
}

void Parser::parseQualifiedUse()
{
  // Every `NAME.` before the last name is a Namespace on the path.
  std::vector<Token> path;
  path.push_back( mCursor->advance() );
  mCursor->advance(); // `.`
  Token name = mCursor->advance();
  while ( mCursor->at( TokenKind::DOT ) && mCursor->peek( 1 ).kind == TokenKind::IDENTIFIER &&
          mCursor->current().location.rawOffset() == name.location.rawOffset() + name.length )
  {
    mCursor->advance(); // `.`
    path.push_back( name );
    name = mCursor->advance();
  }
  diag::SourceSpan span = spanning( path.front().span(), name.span() );

  std::vector<ExpressionPtr> arguments;
  bool quiet = false;
  if ( !mCursor->atLineEnd() )
  {
    do
    {
      ExpressionPtr argument = parseItem();
      quiet = quiet || containsError( *argument );
      span = spanning( span, argument->span );
      arguments.push_back( std::move( argument ) );
    } while ( mCursor->match( TokenKind::COMMA ) );
  }

  mBuilder->useMacro( std::move( path ), name, std::move( arguments ), span );
  endStatement( quiet );
}

bool Parser::atNamespaceEnd() const
{
  if ( !mCursor->at( TokenKind::DOT ) || mCursor->peek( 1 ).kind != TokenKind::IDENTIFIER )
  {
    return false;
  }
  std::string_view const word = textOf( mCursor->peek( 1 ) );
  return word == "endnamespace" || word == "endns";
}

void Parser::parseNamespace( Token directive )
{
  if ( mOpenSection.has_value() || mOpenMacro.has_value() )
  {
    report(
        diag::diagnostic( diag::DiagnosticId::NAMESPACE_NOT_AT_TOP_LEVEL ).at( directive.location, directive.length ) );
    recover();
    return;
  }
  if ( !mCursor->at( TokenKind::IDENTIFIER ) )
  {
    report( diag::diagnostic( diag::DiagnosticId::EXPECTED_NAME )
                .at( directive.location, directive.length )
                .arg( "after", std::string{ textOf( directive ) } ) );
    recover();
    return;
  }

  // `.namespace a.b` is `.namespace a` and `.namespace b`, closed by one
  // `.endnamespace`: a spelling and not a construct of its own.
  std::vector<Token> path;
  path.push_back( mCursor->advance() );
  while ( mCursor->at( TokenKind::DOT ) )
  {
    Token const dot = mCursor->advance();
    if ( !mCursor->at( TokenKind::IDENTIFIER ) )
    {
      report(
          diag::diagnostic( diag::DiagnosticId::EXPECTED_NAME ).at( dot.location, dot.length ).arg( "after", "." ) );
      recover();
      return;
    }
    path.push_back( mCursor->advance() );
  }

  mOpenNamespaces.push_back( directive );
  mBuilder->beginNamespace( std::move( path ), spanning( directive.span(), mCursor->peek( 0 ).span() ) );
  expectLineEnd();
}

void Parser::parseNamespaceEnd( Token directive, std::string_view spelling )
{
  if ( mOpenNamespaces.empty() )
  {
    report( diag::diagnostic( diag::DiagnosticId::UNMATCHED_NAMESPACE_END )
                .at( directive.location, directive.length )
                .arg( "directive", std::string{ spelling } ) );
    recover();
    return;
  }
  bool const outsideProc = mOpenProc.has_value() && mOpenNamespaces.size() <= mNamespacesAtProc;
  if ( mOpenSection.has_value() || mOpenMacro.has_value() || outsideProc )
  {
    // A Namespace holds whole blocks: one that closed in the middle of a
    // Section, or of a Proc, would leave it in two scopes.
    report( diag::diagnostic( diag::DiagnosticId::NAMESPACE_END_INSIDE )
                .at( directive.location, directive.length )
                .arg( "directive", std::string{ spelling } ) );
    recover();
    return;
  }
  mOpenNamespaces.pop_back();
  mBuilder->endNamespace( directive.span() );
  expectLineEnd();
}

void Parser::parseInstruction()
{
  Token const mnemonic = mCursor->advance();

  if ( mCursor->atLineEnd() )
  {
    mBuilder->emitInstruction( mnemonic, OperandShape::NONE, nullptr, mnemonic.span() );
    expectLineEnd();
    return;
  }

  endStatement( parseOperand( mnemonic ) );
}

std::optional<char> Parser::indexRegister() const
{
  if ( !mCursor->at( TokenKind::IDENTIFIER ) )
  {
    return std::nullopt;
  }
  std::string_view const text = textOf( mCursor->current() );
  if ( text == "x" || text == "y" )
  {
    return text.front();
  }
  return std::nullopt;
}

std::optional<std::uint32_t> Parser::matchingParenthesis() const
{
  std::uint32_t depth = 0;
  for ( std::uint32_t ahead = 0;; ++ahead )
  {
    switch ( mCursor->peek( ahead ).kind )
    {
    case TokenKind::LEFT_PAREN:
      ++depth;
      break;
    case TokenKind::RIGHT_PAREN:
      --depth;
      if ( depth == 0 )
      {
        return ahead;
      }
      break;
    case TokenKind::LINE_END:
    case TokenKind::END_OF_FILE:
      return std::nullopt;
    default:
      break;
    }
  }
}

bool Parser::parseIndexSuffix( Token mnemonic, ExpressionPtr operand, diag::SourceSpan span )
{
  bool const quiet = containsError( *operand );
  OperandShape shape = OperandShape::DIRECT;

  if ( mCursor->match( TokenKind::COMMA ) )
  {
    // `,x` or `,y` ending the operand is an index register, wherever it
    // stands. Anything else after the comma makes this a list of operands,
    // which no instruction has and a macro use does — whether the name is a
    // macro is not a question the text settles.
    std::optional<char> const index = indexRegister();
    TokenKind const afterIndex = mCursor->peek( 1 ).kind;
    if ( !index.has_value() || ( afterIndex != TokenKind::LINE_END && afterIndex != TokenKind::END_OF_FILE ) )
    {
      std::vector<ExpressionPtr> arguments;
      bool quietList = quiet;
      arguments.push_back( std::move( operand ) );
      do
      {
        ExpressionPtr argument = parseItem();
        quietList = quietList || containsError( *argument );
        span = spanning( span, argument->span );
        arguments.push_back( std::move( argument ) );
      } while ( mCursor->match( TokenKind::COMMA ) );

      mBuilder->useMacro( {}, mnemonic, std::move( arguments ), span );
      return quietList;
    }

    Token const registerToken = mCursor->advance();
    shape = *index == 'x' ? OperandShape::DIRECT_X : OperandShape::DIRECT_Y;
    span = spanning( span, registerToken.span() );
  }

  mBuilder->emitInstruction( mnemonic, shape, std::move( operand ), span );
  return quiet;
}

bool Parser::parseOperand( Token mnemonic )
{
  if ( mCursor->at( TokenKind::HASH ) )
  {
    mCursor->advance();
    ExpressionPtr operand = parseExpression();
    bool const quiet = containsError( *operand );
    diag::SourceSpan const span = spanning( mnemonic.span(), operand->span );
    mBuilder->emitInstruction( mnemonic, OperandShape::IMMEDIATE, std::move( operand ), span );
    return quiet;
  }

  if ( mCursor->at( TokenKind::LEFT_PAREN ) )
  {
    return parseParenthesisedOperand( mnemonic );
  }

  // `push rest...`: a spread is an item of a list and never an operand, so
  // the statement is a use whatever the name turns out to be.
  ExpressionPtr operand = parseItem();
  diag::SourceSpan const span = spanning( mnemonic.span(), operand->span );
  if ( operand->kind == ExpressionKind::SPREAD )
  {
    bool const quiet = containsError( *operand );
    std::vector<ExpressionPtr> arguments;
    arguments.push_back( std::move( operand ) );
    mBuilder->useMacro( {}, mnemonic, std::move( arguments ), span );
    return quiet;
  }
  return parseIndexSuffix( mnemonic, std::move( operand ), span );
}

bool Parser::parseParenthesisedOperand( Token mnemonic )
{
  // The whole of the addressing mode question: a leading parenthesis indirects
  // only when its match ends the operand or is followed by `, y`. Decided here,
  // before any expression is parsed, so that the expression parser — which the
  // Project file reuses — never learns that addressing modes exist.
  std::optional<std::uint32_t> const match = matchingParenthesis();

  bool indirect = false;
  bool groupingBeforeIndex = false;
  if ( match.has_value() )
  {
    TokenKind const after = mCursor->peek( *match + 1 ).kind;
    if ( after == TokenKind::LINE_END || after == TokenKind::END_OF_FILE )
    {
      indirect = true;
    }
    else if ( after == TokenKind::COMMA )
    {
      Token const index = mCursor->peek( *match + 2 );
      indirect = index.kind == TokenKind::IDENTIFIER && textOf( index ) == "y";
      groupingBeforeIndex = !indirect;
    }
  }

  if ( !indirect )
  {
    if ( groupingBeforeIndex )
    {
      // One character from `(expression,x)` and quietly something else, which
      // is the class of answer this tool exists to remove.
      Token const open = mCursor->current();
      Token const close = mCursor->peek( *match );
      report( diag::diagnostic( diag::DiagnosticId::PARENTHESES_ARE_GROUPING )
                  .at( open.location, close.location.rawOffset() + close.length - open.location.rawOffset() ) );
    }

    ExpressionPtr operand = parseExpression();
    diag::SourceSpan const span = spanning( mnemonic.span(), operand->span );
    return parseIndexSuffix( mnemonic, std::move( operand ), span );
  }

  Token const open = mCursor->advance();
  ExpressionPtr operand = parseExpression();
  bool const quiet = containsError( *operand );
  OperandShape shape = OperandShape::INDIRECT;

  if ( mCursor->match( TokenKind::COMMA ) )
  {
    Token const index = mCursor->current();
    std::optional<char> const which = indexRegister();
    if ( !which.has_value() )
    {
      if ( !quiet )
      {
        report( diag::diagnostic( diag::DiagnosticId::EXPECTED_INDEX_REGISTER )
                    .at( index.location, index.length )
                    .arg( "token", describe( index ) ) );
      }
      return true;
    }

    mCursor->advance();
    if ( *which == 'y' )
    {
      // `(expression,y)` is a shape with no mode behind it. `(expression),y` is
      // what was meant, and the message says so.
      report( diag::diagnostic( diag::DiagnosticId::NO_SUCH_ADDRESSING_MODE )
                  .at( open.location, open.length )
                  .arg( "register", "y" ) );
      return true;
    }
    shape = OperandShape::INDEXED_INDIRECT;
  }

  if ( !mCursor->at( TokenKind::RIGHT_PAREN ) )
  {
    if ( !quiet )
    {
      report( diag::diagnostic( diag::DiagnosticId::UNCLOSED_PARENTHESIS ).at( open.location, open.length ) );
    }
    return true;
  }

  Token const close = mCursor->advance();
  diag::SourceSpan span = spanning( mnemonic.span(), close.span() );

  if ( shape == OperandShape::INDIRECT && mCursor->match( TokenKind::COMMA ) )
  {
    Token const registerToken = mCursor->advance(); // `y`, established by the scan
    shape = OperandShape::INDIRECT_Y;
    span = spanning( span, registerToken.span() );
  }

  mBuilder->emitInstruction( mnemonic, shape, std::move( operand ), span );
  return quiet;
}

void Parser::parseSlot( Token directive )
{
  // `.slot NAME, binding [, placement]`. The Binding is required: whether a
  // use is a `jsr` or a `lda (),y` is too much to guess, and it decides the
  // Cell's shape. The placement defaults to `absolute` as a Section's does.
  if ( !mCursor->at( TokenKind::IDENTIFIER ) )
  {
    report( diag::diagnostic( diag::DiagnosticId::EXPECTED_NAME )
                .at( directive.location, directive.length )
                .arg( "after", std::string{ textOf( directive ) } ) );
    recover();
    return;
  }
  Token const name = mCursor->advance();

  if ( !mCursor->match( TokenKind::COMMA ) || !mCursor->at( TokenKind::IDENTIFIER ) )
  {
    Token const here = mCursor->current();
    report( diag::diagnostic( diag::DiagnosticId::EXPECTED_BINDING )
                .at( here.location, here.length )
                .arg( "name", describe( here ) ) );
    recover();
    return;
  }
  Token const bindingWord = mCursor->advance();
  std::string_view const binding = textOf( bindingWord );
  if ( binding != "pointer" && binding != "vector" )
  {
    report( diag::diagnostic( diag::DiagnosticId::EXPECTED_BINDING )
                .at( bindingWord.location, bindingWord.length )
                .arg( "name", std::string{ binding } ) );
    recover();
    return;
  }

  model::PlacementClass placement = model::PlacementClass::ABSOLUTE;
  if ( mCursor->match( TokenKind::COMMA ) )
  {
    if ( !mCursor->at( TokenKind::IDENTIFIER ) )
    {
      Token const here = mCursor->current();
      report( diag::diagnostic( diag::DiagnosticId::EXPECTED_PLACEMENT )
                  .at( here.location, here.length )
                  .arg( "name", describe( here ) ) );
      recover();
      return;
    }
    Token const placementWord = mCursor->advance();
    std::string_view const word = textOf( placementWord );
    if ( word != "zeropage" && word != "absolute" )
    {
      report( diag::diagnostic( diag::DiagnosticId::EXPECTED_PLACEMENT )
                  .at( placementWord.location, placementWord.length )
                  .arg( "name", std::string{ word } ) );
      recover();
      return;
    }
    placement = word == "zeropage" ? model::PlacementClass::ZEROPAGE : model::PlacementClass::ABSOLUTE;
  }

  mBuilder->declareSlot( name,
                         binding == "pointer" ? model::Binding::POINTER : model::Binding::VECTOR,
                         placement,
                         spanning( directive.span(), name.span() ) );
  expectLineEnd();
}

void Parser::parseImplements( Token directive )
{
  // `.implements SLOT, SYMBOL`: both names, explicitly, since a definition may
  // already have a statement beside it on its line and "the next definition"
  // would be a second positional rule.
  if ( !mCursor->at( TokenKind::IDENTIFIER ) )
  {
    report( diag::diagnostic( diag::DiagnosticId::EXPECTED_NAME )
                .at( directive.location, directive.length )
                .arg( "after", std::string{ textOf( directive ) } ) );
    recover();
    return;
  }
  Token const slot = mCursor->advance();

  if ( !mCursor->match( TokenKind::COMMA ) || !mCursor->at( TokenKind::IDENTIFIER ) )
  {
    report( diag::diagnostic( diag::DiagnosticId::EXPECTED_NAME )
                .at( slot.location, slot.length )
                .arg( "after", std::string{ textOf( slot ) } ) );
    recover();
    return;
  }
  Token const symbol = mCursor->advance();

  mBuilder->implement( slot, symbol, spanning( directive.span(), symbol.span() ) );
  expectLineEnd();
}

void Parser::parseTwoNames( Token directive, void ( Builder::*take )( Token, Token, diag::SourceSpan ) )
{
  // The first name is recognised by position, as `then` is after `.endp`, so
  // nothing here becomes a keyword.
  if ( !mCursor->at( TokenKind::IDENTIFIER ) )
  {
    report( diag::diagnostic( diag::DiagnosticId::EXPECTED_NAME )
                .at( directive.location, directive.length )
                .arg( "after", std::string{ textOf( directive ) } ) );
    recover();
    return;
  }
  Token const format = mCursor->advance();

  if ( !mCursor->at( TokenKind::IDENTIFIER ) )
  {
    report( diag::diagnostic( diag::DiagnosticId::EXPECTED_NAME )
                .at( format.location, format.length )
                .arg( "after", std::string{ textOf( format ) } ) );
    recover();
    return;
  }
  Token const label = mCursor->advance();

  ( mBuilder->*take )( format, label, spanning( directive.span(), label.span() ) );
  expectLineEnd();
}

void Parser::parseDriver( Token directive )
{
  // A role and one or two names after it, every one recognised by position;
  // how many a role takes is the Builder's question, since the grammar does
  // not know the roles.
  if ( !mCursor->at( TokenKind::IDENTIFIER ) )
  {
    report( diag::diagnostic( diag::DiagnosticId::EXPECTED_NAME )
                .at( directive.location, directive.length )
                .arg( "after", std::string{ textOf( directive ) } ) );
    recover();
    return;
  }
  Token const role = mCursor->advance();
  std::vector<Token> names;
  while ( mCursor->at( TokenKind::IDENTIFIER ) && names.size() < 2 )
  {
    names.push_back( mCursor->advance() );
  }
  if ( names.empty() )
  {
    report( diag::diagnostic( diag::DiagnosticId::EXPECTED_NAME )
                .at( role.location, role.length )
                .arg( "after", std::string{ textOf( role ) } ) );
    recover();
    return;
  }
  diag::SourceSpan const span = spanning( directive.span(), names.back().span() );
  mBuilder->declareDriverRole( role, std::move( names ), span );
  expectLineEnd();
}

void Parser::parseTransition( Token directive )
{
  if ( !mCursor->at( TokenKind::IDENTIFIER ) )
  {
    report( diag::diagnostic( diag::DiagnosticId::EXPECTED_NAME )
                .at( directive.location, directive.length )
                .arg( "after", std::string{ textOf( directive ) } ) );
    recover();
    return;
  }

  Token const name = mCursor->advance();
  mBuilder->transition( name, spanning( directive.span(), name.span() ) );
  expectLineEnd();
}

} // namespace nga::syntax
