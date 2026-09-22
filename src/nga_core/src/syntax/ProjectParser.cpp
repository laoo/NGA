#include "nga/syntax/ProjectParser.hpp"

#include "nga/syntax/ExpressionParser.hpp"
#include "nga/syntax/Literal.hpp"

#include <optional>
#include <string>
#include <utility>

namespace nga::syntax
{

ProjectParser::ProjectParser( diag::SourceManager const& sources,
                              TokenCursor& cursor,
                              ProjectBuilder& builder,
                              diag::DiagnosticSink& sink )
    : mSources( &sources ), mCursor( &cursor ), mBuilder( &builder ), mSink( &sink )
{
}

void ProjectParser::report( diag::Diagnostic value ) const
{
  mSink->add( std::move( value ) );
}

std::string_view ProjectParser::textOf( Token token ) const
{
  return mSources->textOf( token.span() );
}

std::string ProjectParser::describe( Token token ) const
{
  if ( token.kind == TokenKind::END_OF_FILE || token.length == 0 )
  {
    return std::string{ nameOf( token.kind ) };
  }
  return std::string{ textOf( token ) };
}

void ProjectParser::skipUnreadable( diag::Diagnostic value, bool& alreadyReported )
{
  if ( !alreadyReported )
  {
    report( std::move( value ) );
    alreadyReported = true;
  }
  // Always consumes one token, so a body of nothing but rubbish still reaches
  // its closing brace instead of spinning.
  mCursor->advance();
}

void ProjectParser::parseDocument()
{
  while ( !mCursor->atEnd() )
  {
    if ( !mCursor->at( TokenKind::IDENTIFIER ) )
    {
      Token const here = mCursor->current();
      report( diag::diagnostic( diag::DiagnosticId::EXPECTED_BLOCK_OR_INCLUDE )
                  .at( here.location, here.length )
                  .arg( "token", describe( here ) ) );
      mCursor->advance();
      continue;
    }

    Token const name = mCursor->advance();
    std::string_view const word = textOf( name );
    if ( word == "include" )
    {
      parseInclude( name );
      continue;
    }
    if ( word == "entry" )
    {
      parseEntry( name );
      continue;
    }
    if ( word == "container" )
    {
      parseContainer( name );
      continue;
    }
    if ( word == "optimize" )
    {
      parseIntent( name );
      continue;
    }
    parseBlock( name );
  }
}

bool ProjectParser::openBody( Token name )
{
  if ( mCursor->match( TokenKind::LEFT_BRACE ) )
  {
    return true;
  }

  report( diag::diagnostic( diag::DiagnosticId::EXPECTED_BLOCK_BODY )
              .at( name.location, name.length )
              .arg( "name", std::string{ textOf( name ) } ) );
  return false;
}

bool ProjectParser::bodyEnded( Token name )
{
  if ( mCursor->match( TokenKind::RIGHT_BRACE ) )
  {
    return true;
  }
  if ( !mCursor->atEnd() )
  {
    return false;
  }

  // Reported where the block opened rather than at the end of the file, for
  // the reason an unclosed `.section` is: the opening line is where the reader
  // has to go.
  report( diag::diagnostic( diag::DiagnosticId::PROJECT_BLOCK_NOT_CLOSED )
              .at( name.location, name.length )
              .arg( "name", std::string{ textOf( name ) } ) );
  return true;
}

void ProjectParser::parseBlock( Token name )
{
  std::string_view const word = textOf( name );
  if ( word == "modules" )
  {
    parseModules( name );
    return;
  }
  if ( word == "diagnostics" )
  {
    parseDiagnostics( name );
    return;
  }
  if ( word == "constants" )
  {
    parseConstants( name );
    return;
  }
  if ( word == "phase" )
  {
    parsePhase( name );
    return;
  }
  if ( word == "resident" )
  {
    parseResident( name );
    return;
  }
  if ( word == "group" )
  {
    parseGroup( name );
    return;
  }
  if ( word == "target" )
  {
    parseTarget( name );
    return;
  }
  if ( word == "storage" )
  {
    parseStorage( name );
    return;
  }
  if ( word == "panes" )
  {
    parsePanes( name );
    return;
  }
  if ( word == "transform" )
  {
    parseTransform( name );
    return;
  }

  report( diag::diagnostic( diag::DiagnosticId::UNKNOWN_PROJECT_BLOCK )
              .at( name.location, name.length )
              .arg( "name", std::string{ word } ) );

  // Read the body anyway, so an unknown block costs one finding rather than one
  // per entry inside it.
  if ( openBody( name ) )
  {
    while ( !bodyEnded( name ) )
    {
      mCursor->advance();
    }
  }
}

void ProjectParser::parseModules( Token name )
{
  // A name before the brace makes the block a group of the Modules it
  // declares, as `phase NAME` makes a block a Phase's. An entry inside the body
  // may begin with an identifier too — a generator call does — but this is
  // before the brace, so position tells the two apart with no lookahead.
  std::optional<Token> group;
  if ( mCursor->at( TokenKind::IDENTIFIER ) )
  {
    group = mCursor->advance();
    mBuilder->declareGroup( *group );
  }

  if ( !openBody( name ) )
  {
    return;
  }

  bool reported = false;
  while ( !bodyEnded( name ) )
  {
    bool const isCall = mCursor->at( TokenKind::IDENTIFIER ) && mCursor->peek( 1 ).kind == TokenKind::LEFT_PAREN;
    if ( !mCursor->at( TokenKind::STRING ) && !isCall )
    {
      Token const here = mCursor->current();
      skipUnreadable( diag::diagnostic( diag::DiagnosticId::EXPECTED_MODULE_ENTRY ).at( here.location, here.length ),
                      reported );
      continue;
    }
    reported = false;

    Token const head = mCursor->advance();
    std::vector<ProjectBuilder::GeneratorArgument> arguments;
    if ( isCall && !parseGeneratorArguments( head, arguments ) )
    {
      continue;
    }

    std::optional<Token> alias;

    // `as` is recognised by position, exactly as `at` is in `.section`, so
    // nothing here becomes a keyword.
    if ( mCursor->at( TokenKind::IDENTIFIER ) && textOf( mCursor->current() ) == "as" )
    {
      Token const marker = mCursor->advance();
      if ( !mCursor->at( TokenKind::IDENTIFIER ) )
      {
        report( diag::diagnostic( diag::DiagnosticId::EXPECTED_ALIAS ).at( marker.location, marker.length ) );
        continue;
      }
      alias = mCursor->advance();
    }

    if ( isCall )
    {
      mBuilder->addGeneratedModule( head, std::move( arguments ), alias, group );
      continue;
    }
    mBuilder->addModule( head, alias, group );
  }
}

void ProjectParser::parseGroup( Token keyword )
{
  if ( !mCursor->at( TokenKind::IDENTIFIER ) )
  {
    Token const here = mCursor->current();
    report( diag::diagnostic( diag::DiagnosticId::EXPECTED_GROUP_NAME )
                .at( keyword.location, keyword.length )
                .arg( "token", describe( here ) ) );

    // A body with no name to attach it to is skipped whole, as a `phase`
    // without one is.
    if ( mCursor->match( TokenKind::LEFT_BRACE ) )
    {
      while ( !bodyEnded( keyword ) )
      {
        mCursor->advance();
      }
    }
    return;
  }

  Token const name = mCursor->advance();
  mBuilder->declareGroup( name );
  std::function<void( Token, Token )> const base = [this, name]( Token window, Token state )
  { mBuilder->groupBase( name, window, state ); };
  parseNameBody(
      name,
      diag::DiagnosticId::EXPECTED_GROUP_ENTRY,
      [this, name]( Token member ) { mBuilder->groupMember( name, member ); },
      &base );
}

void ProjectParser::parseDiagnostics( Token name )
{
  if ( !openBody( name ) )
  {
    return;
  }

  bool reported = false;
  while ( !bodyEnded( name ) )
  {
    Token const here = mCursor->current();
    std::optional<diag::SeverityOverride> action;
    if ( here.kind == TokenKind::IDENTIFIER )
    {
      std::string_view const word = textOf( here );
      if ( word == "deny" )
      {
        action = diag::SeverityOverride::DENY;
      }
      else if ( word == "allow" )
      {
        action = diag::SeverityOverride::ALLOW;
      }
      else if ( word == "off" )
      {
        action = diag::SeverityOverride::OFF;
      }
    }

    if ( !action.has_value() )
    {
      skipUnreadable( diag::diagnostic( diag::DiagnosticId::EXPECTED_SEVERITY )
                          .at( here.location, here.length )
                          .arg( "token", describe( here ) ),
                      reported );
      continue;
    }
    reported = false;
    mCursor->advance();

    if ( !mCursor->at( TokenKind::IDENTIFIER ) )
    {
      Token const missing = mCursor->current();
      report( diag::diagnostic( diag::DiagnosticId::UNKNOWN_DIAGNOSTIC_CODE )
                  .at( missing.location, missing.length )
                  .arg( "code", describe( missing ) ) );
      continue;
    }

    mBuilder->setSeverity( mCursor->advance(), *action );
  }
}

/// `constants { NAME = NUMBER }`. The value is one token by design: a
/// terminator is what an expression would need to end, and this grammar has
/// none — see docs/decisions/0178-a-configuration-is-a-document.md.
void ProjectParser::parseConstants( Token name )
{
  if ( !openBody( name ) )
  {
    return;
  }

  bool reported = false;
  while ( !bodyEnded( name ) )
  {
    Token const here = mCursor->current();
    if ( here.kind != TokenKind::IDENTIFIER )
    {
      skipUnreadable( diag::diagnostic( diag::DiagnosticId::EXPECTED_CONSTANT_ENTRY )
                          .at( here.location, here.length )
                          .arg( "token", describe( here ) ),
                      reported );
      continue;
    }
    reported = false;
    Token const declared = mCursor->advance();

    if ( !mCursor->at( TokenKind::EQUAL ) )
    {
      Token const missing = mCursor->current();
      report( diag::diagnostic( diag::DiagnosticId::EXPECTED_CONSTANT_ENTRY )
                  .at( missing.location, missing.length )
                  .arg( "token", describe( missing ) ) );
      continue;
    }
    mCursor->advance();

    if ( !mCursor->at( TokenKind::NUMBER ) )
    {
      Token const missing = mCursor->current();
      report( diag::diagnostic( diag::DiagnosticId::EXPECTED_CONSTANT_ENTRY )
                  .at( missing.location, missing.length )
                  .arg( "token", describe( missing ) ) );
      // What stood where the value should have costs one finding rather than
      // two: read past it, unless it is the brace that ends the block, which
      // the loop is owed.
      if ( !mCursor->at( TokenKind::RIGHT_BRACE ) )
      {
        mCursor->advance();
      }
      continue;
    }

    mBuilder->addConstant( declared, mCursor->advance() );
  }
}

void ProjectParser::parseNameList( Token after, std::function<void( Token )> const& take )
{
  // A list ends where the commas stop, which is what keeps its arity
  // self-delimiting without a line ending to end it.
  Token previous = after;
  while ( true )
  {
    if ( !mCursor->at( TokenKind::IDENTIFIER ) )
    {
      report( diag::diagnostic( diag::DiagnosticId::EXPECTED_NAME_IN_LIST )
                  .at( previous.location, previous.length )
                  .arg( "after", std::string{ textOf( previous ) } ) );
      return;
    }
    take( mCursor->advance() );
    if ( !mCursor->at( TokenKind::COMMA ) )
    {
      return;
    }
    previous = mCursor->advance();
  }
}

std::vector<Token> ProjectParser::parseDottedName( Token after )
{
  std::vector<Token> path;
  if ( !mCursor->at( TokenKind::IDENTIFIER ) )
  {
    report( diag::diagnostic( diag::DiagnosticId::EXPECTED_NAME_IN_LIST )
                .at( after.location, after.length )
                .arg( "after", std::string{ textOf( after ) } ) );
    return path;
  }
  path.push_back( mCursor->advance() );
  while ( mCursor->at( TokenKind::DOT ) && mCursor->peek( 1 ).kind == TokenKind::IDENTIFIER )
  {
    mCursor->advance();
    path.push_back( mCursor->advance() );
  }
  return path;
}

void ProjectParser::parseQualifiedNameList( Token after, std::function<void( Token, std::vector<Token> )> const& take )
{
  Token previous = after;
  while ( true )
  {
    if ( !mCursor->at( TokenKind::IDENTIFIER ) )
    {
      report( diag::diagnostic( diag::DiagnosticId::EXPECTED_NAME_IN_LIST )
                  .at( previous.location, previous.length )
                  .arg( "after", std::string{ textOf( previous ) } ) );
      return;
    }
    Token const module = mCursor->advance();

    if ( !mCursor->match( TokenKind::DOT ) || !mCursor->at( TokenKind::IDENTIFIER ) )
    {
      Token const here = mCursor->current();
      report( diag::diagnostic( diag::DiagnosticId::EXPECTED_QUALIFIED_NAME )
                  .at( module.location, module.length )
                  .arg( "token", describe( here ) ) );
      return;
    }
    take( module, parseDottedName( module ) );

    if ( !mCursor->at( TokenKind::COMMA ) )
    {
      return;
    }
    previous = mCursor->advance();
  }
}

void ProjectParser::parseTransform( Token keyword )
{
  if ( !mCursor->at( TokenKind::IDENTIFIER ) )
  {
    Token const here = mCursor->current();
    report( diag::diagnostic( diag::DiagnosticId::EXPECTED_TRANSFORM_NAME )
                .at( keyword.location, keyword.length )
                .arg( "token", describe( here ) ) );

    // A body with no name to attach it to is skipped whole, as a `phase`
    // without one is.
    if ( mCursor->match( TokenKind::LEFT_BRACE ) )
    {
      while ( !bodyEnded( keyword ) )
      {
        mCursor->advance();
      }
    }
    return;
  }

  Token const name = mCursor->advance();
  if ( !openBody( name ) )
  {
    return;
  }

  bool reported = false;
  while ( !bodyEnded( name ) )
  {
    Token const here = mCursor->current();
    if ( here.kind != TokenKind::IDENTIFIER )
    {
      skipUnreadable( diag::diagnostic( diag::DiagnosticId::EXPECTED_QUALIFIED_NAME )
                          .at( here.location, here.length )
                          .arg( "token", describe( here ) ),
                      reported );
      continue;
    }
    reported = false;
    parseQualifiedNameList( name,
                            [this, name]( Token module, std::vector<Token> section )
                            { mBuilder->applyTransform( name, module, std::move( section ) ); } );
  }
}

void ProjectParser::parsePhase( Token keyword )
{
  if ( !mCursor->at( TokenKind::IDENTIFIER ) )
  {
    Token const here = mCursor->current();
    report( diag::diagnostic( diag::DiagnosticId::EXPECTED_PHASE_NAME )
                .at( keyword.location, keyword.length )
                .arg( "token", describe( here ) ) );

    // A body with no name to attach it to is skipped whole, so that it costs
    // one finding rather than one per entry inside it.
    if ( mCursor->match( TokenKind::LEFT_BRACE ) )
    {
      while ( !bodyEnded( keyword ) )
      {
        mCursor->advance();
      }
    }
    return;
  }

  Token const name = mCursor->advance();
  if ( !openBody( name ) )
  {
    return;
  }
  mBuilder->declarePhase( name );

  bool reported = false;
  while ( !bodyEnded( name ) )
  {
    Token const here = mCursor->current();
    std::string_view const word = here.kind == TokenKind::IDENTIFIER ? textOf( here ) : std::string_view{};

    // `needs` and `then` are recognised by position, as `as` and `at` are, so
    // neither becomes a keyword.
    if ( word == "needs" )
    {
      reported = false;
      mCursor->advance();
      parseNameList( here, [this, name]( Token module ) { mBuilder->phaseNeeds( name, module ); } );
      continue;
    }
    if ( word == "then" )
    {
      reported = false;
      mCursor->advance();
      parseNameList( here, [this, name]( Token next ) { mBuilder->phaseLeadsTo( name, next ); } );
      continue;
    }
    if ( word == "entry" )
    {
      reported = false;
      mCursor->advance();
      if ( std::vector<Token> label = parseDottedName( here ); !label.empty() )
      {
        mBuilder->phaseEntry( name, std::move( label ) );
      }
      continue;
    }
    if ( word == "base" )
    {
      reported = false;
      mCursor->advance();
      parseBase( here, [this, name]( Token window, Token state ) { mBuilder->phaseBase( name, window, state ); } );
      continue;
    }

    skipUnreadable( diag::diagnostic( diag::DiagnosticId::EXPECTED_PHASE_ENTRY )
                        .at( here.location, here.length )
                        .arg( "token", describe( here ) ),
                    reported );
  }
}

void ProjectParser::parseResident( Token name )
{
  parseNameBody(
      name, diag::DiagnosticId::EXPECTED_RESIDENT_ENTRY, [this]( Token module ) { mBuilder->addResident( module ); } );
}

void ProjectParser::parseBase( Token keyword, std::function<void( Token, Token )> const& take )
{
  // `base WINDOW = STATE`: two names around `=`, each recognised by position.
  if ( !mCursor->at( TokenKind::IDENTIFIER ) || mCursor->peek( 1 ).kind != TokenKind::EQUAL ||
       mCursor->peek( 2 ).kind != TokenKind::IDENTIFIER )
  {
    Token const here = mCursor->current();
    report( diag::diagnostic( diag::DiagnosticId::EXPECTED_BASE_FORM )
                .at( keyword.location, keyword.length )
                .arg( "token", describe( here ) ) );
    return;
  }
  Token const window = mCursor->advance();
  mCursor->advance();
  Token const state = mCursor->advance();
  take( window, state );
}

void ProjectParser::parseNameBody( Token name,
                                   diag::DiagnosticId entryError,
                                   std::function<void( Token )> const& take,
                                   std::function<void( Token, Token )> const* base )
{
  if ( !openBody( name ) )
  {
    return;
  }

  bool reported = false;
  std::optional<Token> last;
  while ( !bodyEnded( name ) )
  {
    Token const here = mCursor->current();
    if ( base != nullptr && here.kind == TokenKind::IDENTIFIER && textOf( here ) == "base" )
    {
      // A group may carry a base beside its list, which the Phases needing it
      // inherit — see docs/decisions/0056-a-phase-chooses-a-base.md.
      reported = false;
      last.reset();
      mCursor->advance();
      parseBase( here, *base );
      continue;
    }
    if ( here.kind != TokenKind::IDENTIFIER )
    {
      skipUnreadable( diag::diagnostic( entryError ).at( here.location, here.length ).arg( "token", describe( here ) ),
                      reported );
      continue;
    }
    reported = false;

    if ( last.has_value() )
    {
      // Two names with nothing between them. Reported once and both taken, so
      // that a missing comma costs one finding rather than a Module that
      // nobody named.
      report( diag::diagnostic( diag::DiagnosticId::EXPECTED_COMMA )
                  .at( here.location, here.length )
                  .arg( "previous", std::string{ textOf( *last ) } )
                  .arg( "next", std::string{ textOf( here ) } ) );
    }
    parseNameList( name,
                   [&take, &last]( Token entry )
                   {
                     take( entry );
                     last = entry;
                   } );
  }
}

bool ProjectParser::atValueStart() const
{
  switch ( mCursor->kind() )
  {
  case TokenKind::NUMBER:
  case TokenKind::CHARACTER:
  case TokenKind::STRING:
  case TokenKind::IDENTIFIER:
  case TokenKind::LOCAL_IDENTIFIER:
  case TokenKind::LEFT_PAREN:
  case TokenKind::MINUS:
  case TokenKind::TILDE:
  case TokenKind::BANG:
  case TokenKind::LESS:
  case TokenKind::GREATER:
    return true;
  default:
    return false;
  }
}

bool ProjectParser::rejectNames( Expression const& node ) const
{
  // A value in the Project is written out: there are no Symbols for a name
  // to mean, and saying so here is what keeps the finding from being "not
  // defined". An attribute's receiver is the name, so it is what gets named.
  if ( node.kind == ExpressionKind::NAME || node.kind == ExpressionKind::LOCAL_NAME )
  {
    report( diag::diagnostic( diag::DiagnosticId::VALUE_NAMES_SOMETHING )
                .at( node.token.location, node.token.length )
                .arg( "name", std::string{ textOf( node.token ) } ) );
    return true;
  }
  bool any = false;
  if ( node.left != nullptr && rejectNames( *node.left ) )
  {
    any = true;
  }
  if ( node.right != nullptr && rejectNames( *node.right ) )
  {
    any = true;
  }
  return any;
}

void ProjectParser::skipCallEnd()
{
  // Recovery is by entry and not by token: read to the `)` that closes the
  // call, then past the `as NAME` that may follow it, so that the loop resumes
  // on an entry rather than on the tail of the one that failed.
  while ( !mCursor->atEnd() && !mCursor->at( TokenKind::RIGHT_BRACE ) )
  {
    if ( mCursor->match( TokenKind::RIGHT_PAREN ) )
    {
      break;
    }
    mCursor->advance();
  }

  if ( mCursor->at( TokenKind::IDENTIFIER ) && textOf( mCursor->current() ) == "as" )
  {
    mCursor->advance();
    if ( mCursor->at( TokenKind::IDENTIFIER ) )
    {
      mCursor->advance();
    }
  }
}

bool ProjectParser::parseGeneratorArguments( Token generator, std::vector<ProjectBuilder::GeneratorArgument>& into )
{
  Token const open = mCursor->advance(); // `(`

  if ( mCursor->match( TokenKind::RIGHT_PAREN ) )
  {
    return true;
  }

  Token previous = open;
  while ( true )
  {
    ProjectBuilder::GeneratorArgument argument;
    diag::SourceSpan span{};

    // `NAME = value` and a flag are told apart by one token of lookahead, and a
    // value standing alone is the positional argument.
    if ( mCursor->at( TokenKind::IDENTIFIER ) && mCursor->peek( 1 ).kind == TokenKind::EQUAL )
    {
      argument.name = mCursor->advance();
      previous = mCursor->advance(); // `=`
      span = argument.name->span();
    }
    else if ( mCursor->at( TokenKind::IDENTIFIER ) )
    {
      // A name with nothing after it is a flag: `root`, `movable`.
      Token const flag = mCursor->advance();
      argument.name = flag;
      argument.span = flag.span();
      into.push_back( std::move( argument ) );
      previous = flag;
      if ( mCursor->match( TokenKind::COMMA ) )
      {
        continue;
      }
      break;
    }

    if ( mCursor->at( TokenKind::STRING ) )
    {
      argument.literal = mCursor->advance();
      span = argument.name.has_value() ? spanning( span, argument.literal->span() ) : argument.literal->span();
      previous = *argument.literal;
    }
    else if ( mCursor->at( TokenKind::IDENTIFIER ) &&
              ( mCursor->peek( 1 ).kind == TokenKind::COMMA || mCursor->peek( 1 ).kind == TokenKind::RIGHT_PAREN ) )
    {
      argument.word = mCursor->advance();
      span = argument.name.has_value() ? spanning( span, argument.word->span() ) : argument.word->span();
      previous = *argument.word;
    }
    else
    {
      argument.value = parseValue( previous );
      if ( argument.value == nullptr )
      {
        skipCallEnd();
        return false;
      }
      span = argument.name.has_value() ? spanning( span, argument.value->span ) : argument.value->span;
    }

    argument.span = span;
    into.push_back( std::move( argument ) );

    if ( !mCursor->match( TokenKind::COMMA ) )
    {
      break;
    }
  }

  if ( !mCursor->match( TokenKind::RIGHT_PAREN ) )
  {
    Token const here = mCursor->current();
    report( diag::diagnostic( diag::DiagnosticId::EXPECTED_CALL_END )
                .at( generator.location, generator.length )
                .arg( "token", describe( here ) ) );
    skipCallEnd();
    return false;
  }
  return true;
}

ExpressionPtr ProjectParser::parseValue( Token after )
{
  if ( !atValueStart() )
  {
    report( diag::diagnostic( diag::DiagnosticId::EXPECTED_VALUE )
                .at( after.location, after.length )
                .arg( "after", std::string{ textOf( after ) } ) );
    return nullptr;
  }

  ExpressionParser expressions{ *mCursor, *mSink };
  ExpressionPtr value = expressions.parse();
  if ( containsError( *value ) || rejectNames( *value ) )
  {
    return nullptr;
  }
  return value;
}

void ProjectParser::parseValueList( Token after, std::function<void( ExpressionPtr )> const& take )
{
  Token previous = after;
  while ( true )
  {
    if ( !atValueStart() )
    {
      report( diag::diagnostic( diag::DiagnosticId::EXPECTED_VALUE )
                  .at( previous.location, previous.length )
                  .arg( "after", std::string{ textOf( previous ) } ) );
      return;
    }
    // A value that could not be read has been reported and consumed, and the
    // list goes on past it, so one bad value costs one finding.
    if ( ExpressionPtr value = parseValue( previous ); value != nullptr )
    {
      take( std::move( value ) );
    }
    if ( !mCursor->at( TokenKind::COMMA ) )
    {
      return;
    }
    previous = mCursor->advance();
  }
}

std::optional<std::pair<ExpressionPtr, ExpressionPtr>> ProjectParser::parseRange( Token after )
{
  ExpressionPtr begin = parseValue( after );
  Token const dots = mCursor->current();
  if ( !mCursor->match( TokenKind::DOT_DOT ) )
  {
    report( diag::diagnostic( diag::DiagnosticId::EXPECTED_RANGE ).at( after.location, after.length ) );
    return std::nullopt;
  }
  ExpressionPtr end = parseValue( dots );
  if ( begin == nullptr || end == nullptr )
  {
    return std::nullopt;
  }
  return std::make_pair( std::move( begin ), std::move( end ) );
}

void ProjectParser::parseRegion( Token keyword )
{
  // A name is optional and a value never is one, so an identifier here can
  // only be the name.
  std::optional<Token> name;
  if ( mCursor->at( TokenKind::IDENTIFIER ) )
  {
    name = mCursor->advance();
  }
  std::optional<std::pair<ExpressionPtr, ExpressionPtr>> range = parseRange( name.value_or( keyword ) );
  if ( !range.has_value() )
  {
    return;
  }
  if ( !mCursor->at( TokenKind::IDENTIFIER ) )
  {
    // Reported at the entry and not at what followed it, which is usually
    // the closing brace on the next line.
    report( diag::diagnostic( diag::DiagnosticId::EXPECTED_REGION_PROPERTY )
                .at( keyword.location, keyword.length )
                .arg( "token", describe( mCursor->current() ) ) );
    return;
  }
  mBuilder->addRegion( keyword, name, std::move( range->first ), std::move( range->second ), mCursor->advance() );
}

void ProjectParser::parseRegister( Token keyword )
{
  if ( !mCursor->at( TokenKind::IDENTIFIER ) )
  {
    report( diag::diagnostic( diag::DiagnosticId::EXPECTED_NAME_IN_LIST )
                .at( keyword.location, keyword.length )
                .arg( "after", std::string{ textOf( keyword ) } ) );
    // The address is read past anyway, so the entry costs one finding.
    if ( atValueStart() )
    {
      (void)parseValue( keyword );
    }
    return;
  }
  Token const name = mCursor->advance();
  ExpressionPtr address = parseValue( name );
  if ( address == nullptr )
  {
    return;
  }
  ExpressionPtr width;
  if ( Token const comma = mCursor->current(); mCursor->match( TokenKind::COMMA ) )
  {
    width = parseValue( comma );
    if ( width == nullptr )
    {
      return;
    }
  }
  mBuilder->addRegister( keyword, name, std::move( address ), std::move( width ) );
}

void ProjectParser::parseTarget( Token name )
{
  if ( !openBody( name ) )
  {
    return;
  }

  bool reported = false;
  while ( !bodyEnded( name ) )
  {
    Token const here = mCursor->current();
    std::string_view const word = here.kind == TokenKind::IDENTIFIER ? textOf( here ) : std::string_view{};

    if ( word == "region" )
    {
      reported = false;
      mCursor->advance();
      parseRegion( here );
      continue;
    }
    if ( word == "register" )
    {
      reported = false;
      mCursor->advance();
      parseRegister( here );
      continue;
    }
    if ( word == "units" )
    {
      reported = false;
      mCursor->advance();
      parseUnitSet( here );
      continue;
    }
    if ( word == "window" )
    {
      reported = false;
      mCursor->advance();
      parseWindow( here );
      continue;
    }
    if ( word == "containers" )
    {
      reported = false;
      mCursor->advance();
      parseNameList( here, [this, here]( Token taken ) { mBuilder->addAcceptedContainer( here, taken ); } );
      continue;
    }
    if ( word == "cpu" )
    {
      reported = false;
      mCursor->advance();
      // Quoted, because a processor's name begins with a digit and no
      // identifier of this language may — see
      // docs/decisions/0182-the-target-names-its-processor.md.
      if ( mCursor->at( TokenKind::STRING ) )
      {
        mBuilder->setCpu( here, mCursor->advance() );
      }
      else
      {
        report( diag::diagnostic( diag::DiagnosticId::EXPECTED_NAME_IN_LIST )
                    .at( here.location, here.length )
                    .arg( "after", std::string{ textOf( here ) } ) );
      }
      continue;
    }

    skipUnreadable( diag::diagnostic( diag::DiagnosticId::EXPECTED_TARGET_ENTRY )
                        .at( here.location, here.length )
                        .arg( "token", describe( here ) ),
                    reported );
  }
}

void ProjectParser::parseUnitSet( Token keyword )
{
  if ( !mCursor->at( TokenKind::IDENTIFIER ) )
  {
    report( diag::diagnostic( diag::DiagnosticId::EXPECTED_NAME_IN_LIST )
                .at( keyword.location, keyword.length )
                .arg( "after", std::string{ textOf( keyword ) } ) );
    if ( atValueStart() )
    {
      (void)parseValue( keyword );
    }
    return;
  }
  Token const name = mCursor->advance();
  ExpressionPtr count = parseValue( name );
  if ( count == nullptr )
  {
    return;
  }
  mBuilder->addUnitSet( keyword, name, std::move( count ) );
}

void ProjectParser::parseWindow( Token keyword )
{
  // The name is read, or its absence reported, and the rest of the entry is
  // read past either way, so that a window without a name costs one finding.
  std::optional<Token> name;
  if ( mCursor->at( TokenKind::IDENTIFIER ) )
  {
    name = mCursor->advance();
  }
  else
  {
    report( diag::diagnostic( diag::DiagnosticId::EXPECTED_NAME_IN_LIST )
                .at( keyword.location, keyword.length )
                .arg( "after", std::string{ textOf( keyword ) } ) );
  }

  // The ranges, comma-separated, and then `views` — recognised by position,
  // as `at` is after a placement, so nothing here becomes a keyword.
  std::vector<std::pair<ExpressionPtr, ExpressionPtr>> ranges;
  Token previous = name.value_or( keyword );
  while ( true )
  {
    std::optional<std::pair<ExpressionPtr, ExpressionPtr>> range = parseRange( previous );
    if ( !range.has_value() )
    {
      return;
    }
    ranges.push_back( std::move( *range ) );
    if ( !mCursor->at( TokenKind::COMMA ) )
    {
      break;
    }
    previous = mCursor->advance();
  }

  if ( !( mCursor->at( TokenKind::IDENTIFIER ) && textOf( mCursor->current() ) == "views" ) )
  {
    report( diag::diagnostic( diag::DiagnosticId::EXPECTED_VIEWS )
                .at( keyword.location, keyword.length )
                .arg( "token", describe( mCursor->current() ) ) );
    return;
  }
  Token const views = mCursor->advance();
  std::vector<Token> states;
  parseNameList( views, [&states]( Token state ) { states.push_back( state ); } );
  if ( states.empty() )
  {
    return;
  }

  std::optional<Token> base;
  if ( mCursor->at( TokenKind::IDENTIFIER ) && textOf( mCursor->current() ) == "base" )
  {
    Token const word = mCursor->advance();
    if ( !mCursor->at( TokenKind::IDENTIFIER ) )
    {
      report( diag::diagnostic( diag::DiagnosticId::EXPECTED_NAME_IN_LIST )
                  .at( word.location, word.length )
                  .arg( "after", std::string{ textOf( word ) } ) );
      return;
    }
    base = mCursor->advance();
  }
  if ( name.has_value() )
  {
    mBuilder->addWindow( keyword, *name, std::move( ranges ), std::move( states ), base );
  }
}

void ProjectParser::parsePanes( Token name )
{
  // `in WINDOW`, then `= STATE` where the Panes are pinned to a named
  // state, then the body: names, each with `[N]` for a family. `in` is
  // recognised by position, as every word here is.
  if ( !mCursor->at( TokenKind::IDENTIFIER ) || textOf( mCursor->current() ) != "in" ||
       mCursor->peek( 1 ).kind != TokenKind::IDENTIFIER )
  {
    Token const here = mCursor->current();
    report( diag::diagnostic( diag::DiagnosticId::EXPECTED_PANES_HEADER )
                .at( name.location, name.length )
                .arg( "token", describe( here ) ) );
    skipBody( name );
    return;
  }
  mCursor->advance();
  Token const window = mCursor->advance();
  std::optional<Token> state;
  if ( Token const equals = mCursor->current(); mCursor->match( TokenKind::EQUAL ) )
  {
    if ( !mCursor->at( TokenKind::IDENTIFIER ) )
    {
      report( diag::diagnostic( diag::DiagnosticId::EXPECTED_NAME_IN_LIST )
                  .at( equals.location, equals.length )
                  .arg( "after", std::string{ textOf( equals ) } ) );
      skipBody( name );
      return;
    }
    state = mCursor->advance();
  }
  mBuilder->beginPanes( name, window, state );

  if ( !openBody( name ) )
  {
    return;
  }
  bool reported = false;
  bool afterComma = false;
  while ( true )
  {
    // A comma promises a name: one before the brace is reported, as every
    // list of names reports it.
    if ( afterComma && mCursor->at( TokenKind::RIGHT_BRACE ) )
    {
      Token const here = mCursor->current();
      report( diag::diagnostic( diag::DiagnosticId::EXPECTED_PANE_ENTRY )
                  .at( here.location, here.length )
                  .arg( "token", describe( here ) ) );
    }
    if ( bodyEnded( name ) )
    {
      break;
    }
    afterComma = false;
    if ( !mCursor->at( TokenKind::IDENTIFIER ) )
    {
      Token const here = mCursor->current();
      skipUnreadable( diag::diagnostic( diag::DiagnosticId::EXPECTED_PANE_ENTRY )
                          .at( here.location, here.length )
                          .arg( "token", describe( here ) ),
                      reported );
      continue;
    }
    reported = false;
    Token const pane = mCursor->advance();
    ExpressionPtr count;
    if ( Token const open = mCursor->current(); mCursor->match( TokenKind::LEFT_BRACKET ) )
    {
      count = parseValue( open );
      if ( count == nullptr )
      {
        continue;
      }
      if ( !mCursor->match( TokenKind::RIGHT_BRACKET ) )
      {
        Token const here = mCursor->current();
        report( diag::diagnostic( diag::DiagnosticId::EXPECTED_PANE_ENTRY )
                    .at( here.location, here.length )
                    .arg( "token", describe( here ) ) );
        continue;
      }
    }
    mBuilder->addPane( pane, std::move( count ) );
    if ( mCursor->match( TokenKind::COMMA ) )
    {
      afterComma = true;
    }
    else if ( !mCursor->at( TokenKind::RIGHT_BRACE ) )
    {
      Token const here = mCursor->current();
      skipUnreadable( diag::diagnostic( diag::DiagnosticId::EXPECTED_PANE_ENTRY )
                          .at( here.location, here.length )
                          .arg( "token", describe( here ) ),
                      reported );
    }
  }
}

void ProjectParser::skipBody( Token name )
{
  // Read on to the body and through it, so that a block whose header could
  // not be read costs one finding and not one per entry inside it.
  while ( !mCursor->atEnd() && !mCursor->at( TokenKind::LEFT_BRACE ) )
  {
    mCursor->advance();
  }
  if ( mCursor->match( TokenKind::LEFT_BRACE ) )
  {
    while ( !bodyEnded( name ) )
    {
      mCursor->advance();
    }
  }
}

void ProjectParser::parseStorage( Token name )
{
  if ( !openBody( name ) )
  {
    return;
  }

  bool reported = false;
  while ( !bodyEnded( name ) )
  {
    Token const here = mCursor->current();
    std::string_view const word = here.kind == TokenKind::IDENTIFIER ? textOf( here ) : std::string_view{};

    if ( word == "units" && mCursor->peek( 1 ).kind == TokenKind::IDENTIFIER )
    {
      // A name is never a value, so `units` followed by one names a set of
      // the target, and followed by a value counts them.
      reported = false;
      mCursor->advance();
      mBuilder->setStorageUnits( here, mCursor->advance() );
      continue;
    }
    if ( word == "units" || word == "size" )
    {
      reported = false;
      mCursor->advance();
      if ( ExpressionPtr value = parseValue( here ); value != nullptr )
      {
        if ( word == "units" )
        {
          mBuilder->setUnitCount( here, std::move( value ) );
        }
        else
        {
          mBuilder->setUnitSize( here, std::move( value ) );
        }
      }
      continue;
    }

    skipUnreadable( diag::diagnostic( diag::DiagnosticId::EXPECTED_STORAGE_ENTRY )
                        .at( here.location, here.length )
                        .arg( "token", describe( here ) ),
                    reported );
  }
}

void ProjectParser::parseEntry( Token keyword )
{
  if ( !mCursor->at( TokenKind::IDENTIFIER ) )
  {
    report( diag::diagnostic( diag::DiagnosticId::EXPECTED_NAME_IN_LIST )
                .at( keyword.location, keyword.length )
                .arg( "after", std::string{ textOf( keyword ) } ) );
    return;
  }
  mBuilder->setEntry( mCursor->advance() );
}

void ProjectParser::parseContainer( Token keyword )
{
  if ( !mCursor->at( TokenKind::IDENTIFIER ) )
  {
    report( diag::diagnostic( diag::DiagnosticId::EXPECTED_NAME_IN_LIST )
                .at( keyword.location, keyword.length )
                .arg( "after", std::string{ textOf( keyword ) } ) );
    return;
  }
  mBuilder->setContainer( mCursor->advance() );
}

void ProjectParser::parseIntent( Token keyword )
{
  if ( !mCursor->at( TokenKind::IDENTIFIER ) )
  {
    report( diag::diagnostic( diag::DiagnosticId::EXPECTED_NAME_IN_LIST )
                .at( keyword.location, keyword.length )
                .arg( "after", std::string{ textOf( keyword ) } ) );
    return;
  }
  mBuilder->setIntent( mCursor->advance() );
}

void ProjectParser::parseInclude( Token keyword )
{
  if ( !mCursor->at( TokenKind::STRING ) )
  {
    Token const here = mCursor->current();
    report( diag::diagnostic( diag::DiagnosticId::EXPECTED_BLOCK_OR_INCLUDE )
                .at( keyword.location, keyword.length )
                .arg( "token", describe( here ) ) );
    return;
  }
  mBuilder->includeDocument( mCursor->advance() );
}

} // namespace nga::syntax
