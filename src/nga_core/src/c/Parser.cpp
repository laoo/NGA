#include "nga/c/Parser.hpp"

#include "nga/c/Lexer.hpp"
#include "nga/syntax/Literal.hpp"

#include <algorithm>
#include <string>
#include <utility>

namespace nga::c
{

namespace
{

diag::SourceSpan spanning( diag::SourceSpan first, diag::SourceSpan last )
{
  std::uint32_t const end = last.begin.rawOffset() + last.length;
  return diag::SourceSpan{ .begin = first.begin, .length = end - first.begin.rawOffset() };
}

bool canBeginExpression( Token const& token )
{
  switch ( token.kind )
  {
  case TokenKind::IDENTIFIER:
  case TokenKind::INTEGER_CONSTANT:
  case TokenKind::STRING_LITERAL:
  case TokenKind::CHARACTER_CONSTANT:
  case TokenKind::LEFT_PAREN:
  case TokenKind::MINUS:
  case TokenKind::TILDE:
  case TokenKind::BANG:
  case TokenKind::STAR:
  case TokenKind::AMPERSAND:
  case TokenKind::PLUS_PLUS:
  case TokenKind::MINUS_MINUS:
    return true;
  case TokenKind::KEYWORD:
    return token.keyword == Keyword::TRUE_CONSTANT || token.keyword == Keyword::FALSE_CONSTANT ||
           token.keyword == Keyword::NULLPTR || token.keyword == Keyword::SIZEOF;
  default:
    return false;
  }
}

bool isType( Keyword keyword )
{
  return keyword == Keyword::U8 || keyword == Keyword::I8 || keyword == Keyword::U16 || keyword == Keyword::I16 ||
         keyword == Keyword::BOOL;
}

/// The lowest precedence a binary operator of the grammar has.
constexpr int LOWEST_PRECEDENCE = 1;

/// How tightly a binary operator binds, as C's grammar orders its levels, or
/// zero for a token that is none.
int precedenceOf( TokenKind kind )
{
  switch ( kind )
  {
  case TokenKind::STAR:
  case TokenKind::SLASH:
  case TokenKind::PERCENT:
    return 10;
  case TokenKind::PLUS:
  case TokenKind::MINUS:
    return 9;
  case TokenKind::LESS_LESS:
  case TokenKind::GREATER_GREATER:
    return 8;
  case TokenKind::LESS:
  case TokenKind::GREATER:
  case TokenKind::LESS_EQUAL:
  case TokenKind::GREATER_EQUAL:
    return 7;
  case TokenKind::EQUAL_EQUAL:
  case TokenKind::BANG_EQUAL:
    return 6;
  case TokenKind::AMPERSAND:
    return 5;
  case TokenKind::CARET:
    return 4;
  case TokenKind::PIPE:
    return 3;
  case TokenKind::AMPERSAND_AMPERSAND:
    return 2;
  case TokenKind::PIPE_PIPE:
    return LOWEST_PRECEDENCE;
  default:
    return 0;
  }
}

ExpressionPtr makeExpression( ExpressionKind kind, Token const& token, diag::SourceSpan span )
{
  auto node = std::make_unique<Expression>();
  node->kind = kind;
  node->token = token;
  node->span = span;
  return node;
}

/// The value of one of the six escapes, the character after `\`.
std::int64_t escapedValue( char escaped )
{
  switch ( escaped )
  {
  case 'n':
    return '\n';
  case 't':
    return '\t';
  case '0':
    return 0;
  default:
    return static_cast<unsigned char>( escaped );
  }
}

/// A character of a literal as a character constant of its own: spelled for
/// the assembler, `screen'a'`, and its ASCII value where the literal has no
/// prefix. `text` is the character as the source writes it, an escape
/// included.
ExpressionPtr literalCharacter( Token const& token, std::string_view prefix, std::string_view text )
{
  ExpressionPtr node = makeExpression( ExpressionKind::CHARACTER_CONSTANT, token, token.span() );
  // A `'` stands alone in a string and is escaped in a character constant.
  std::string const inner = text == "'" ? std::string{ "\\'" } : std::string{ text };
  node->spelling = std::string{ prefix } + "'" + inner + "'";
  node->value = text.size() == 2 && text[0] == '\\' ? escapedValue( text[1] ) : static_cast<unsigned char>( text[0] );
  return node;
}

} // namespace

/// The operator a compound assignment's token holds, `+` of `+=`, and
/// nothing for anything else — see
/// docs/decisions/0158-a-compound-assignment-and-a-step.md.
std::optional<TokenKind> compounded( TokenKind kind )
{
  switch ( kind )
  {
  case TokenKind::PLUS_EQUAL:
    return TokenKind::PLUS;
  case TokenKind::MINUS_EQUAL:
    return TokenKind::MINUS;
  case TokenKind::STAR_EQUAL:
    return TokenKind::STAR;
  case TokenKind::SLASH_EQUAL:
    return TokenKind::SLASH;
  case TokenKind::PERCENT_EQUAL:
    return TokenKind::PERCENT;
  case TokenKind::AMPERSAND_EQUAL:
    return TokenKind::AMPERSAND;
  case TokenKind::CARET_EQUAL:
    return TokenKind::CARET;
  case TokenKind::PIPE_EQUAL:
    return TokenKind::PIPE;
  case TokenKind::LESS_LESS_EQUAL:
    return TokenKind::LESS_LESS;
  case TokenKind::GREATER_GREATER_EQUAL:
    return TokenKind::GREATER_GREATER;
  default:
    return std::nullopt;
  }
}

ExpressionPtr copyOf( Expression const& node )
{
  auto copy = std::make_unique<Expression>();
  copy->kind = node.kind;
  copy->parenthesised = node.parenthesised;
  copy->compound = node.compound;
  copy->writtenAfter = node.writtenAfter;
  copy->depth = node.depth;
  copy->token = node.token;
  copy->span = node.span;
  copy->value = node.value;
  copy->pointer = node.pointer;
  copy->pointeeConst = node.pointeeConst;
  copy->pointeeVolatile = node.pointeeVolatile;
  copy->left = node.left == nullptr ? nullptr : copyOf( *node.left );
  copy->right = node.right == nullptr ? nullptr : copyOf( *node.right );
  copy->arguments.reserve( node.arguments.size() );
  for ( ExpressionPtr const& argument : node.arguments )
  {
    copy->arguments.push_back( argument == nullptr ? nullptr : copyOf( *argument ) );
  }
  copy->spelling = node.spelling;
  return copy;
}

Parser::Parser( diag::SourceManager const& sources, std::span<Token const> tokens, diag::DiagnosticSink& sink )
    : mSources( &sources ), mSink( &sink )
{
  // Comments are the suite's, which reads them for its markers, and nothing of
  // the grammar's.
  for ( Token const& token : tokens )
  {
    if ( token.kind != TokenKind::COMMENT )
    {
      mTokens.push_back( token );
    }
  }
  if ( mTokens.empty() || mTokens.back().kind != TokenKind::END_OF_FILE )
  {
    mTokens.push_back( Token{} );
  }
}

Token Parser::advance()
{
  Token const taken = peek();
  if ( mIndex + 1 < mTokens.size() )
  {
    ++mIndex;
  }
  return taken;
}

bool Parser::match( TokenKind wanted )
{
  if ( !at( wanted ) )
  {
    return false;
  }
  advance();
  return true;
}

void Parser::fail( std::string_view expected )
{
  Token const& found = peek();
  if ( found.kind == TokenKind::UNKNOWN )
  {
    return;
  }
  std::string shown = found.kind == TokenKind::END_OF_FILE
                          ? std::string{ "the end of the file" }
                          : "`" + std::string{ mSources->textOf( found.span() ) } + "`";
  mSink->add( diagnostic( diag::DiagnosticId::C_EXPECTED )
                  .at( found.location, found.length )
                  .arg( "expected", std::string{ expected } )
                  .arg( "found", std::move( shown ) ) );
}

void Parser::refuseDepth( Token const& where, std::string_view what, std::uint32_t limit )
{
  mSink->add( diagnostic( diag::DiagnosticId::C_NESTING_TOO_DEEP )
                  .at( where.location, where.length )
                  .arg( "what", std::string{ what } )
                  .arg( "limit", std::int64_t{ limit } ) );
}

void Parser::recover( bool atTopLevel )
{
  std::uint32_t depth = 0;
  while ( !at( TokenKind::END_OF_FILE ) )
  {
    TokenKind const kind = peek().kind;
    if ( kind == TokenKind::SEMICOLON && depth == 0 )
    {
      advance();
      return;
    }
    if ( kind == TokenKind::LEFT_BRACE )
    {
      ++depth;
    }
    else if ( kind == TokenKind::RIGHT_BRACE )
    {
      if ( depth == 0 )
      {
        if ( atTopLevel )
        {
          advance();
        }
        return;
      }
      if ( --depth == 0 )
      {
        advance();
        // A declaration that failed on a brace, `u8 x = { 1 };`, ends at the
        // `;` after it, which would otherwise be a second finding.
        if ( atTopLevel )
        {
          match( TokenKind::SEMICOLON );
        }
        return;
      }
    }
    advance();
  }
}

TranslationUnit Parser::parseTranslationUnit()
{
  TranslationUnit unit;
  while ( !at( TokenKind::END_OF_FILE ) )
  {
    std::optional<std::vector<Attribute>> attributes = parseAttributes();
    if ( !attributes.has_value() )
    {
      continue;
    }
    bool const attributed = !attributes->empty();
    // `inline` is a specifier of its own, and C lets it stand on either side
    // of the storage one.
    std::optional<Token> inlined;
    std::optional<Token> storage;
    for ( bool more = true; more; )
    {
      if ( atKeyword( Keyword::INLINE ) && !inlined.has_value() )
      {
        inlined = advance();
        continue;
      }
      if ( !storage.has_value() &&
           ( atKeyword( Keyword::STATIC ) || atKeyword( Keyword::EXTERN ) || atKeyword( Keyword::TYPEDEF ) ) )
      {
        storage = advance();
        continue;
      }
      more = false;
    }

    // `inline` says what to do with a body, so everything here that has none
    // says so and goes on: a declaration is still read, and its own findings
    // are worth more than this one repeated.
    auto const withoutBody = [this, &inlined]( std::string_view what )
    {
      if ( inlined.has_value() )
      {
        mSink->add( diagnostic( diag::DiagnosticId::C_INLINE_WITHOUT_BODY )
                        .at( inlined->location, inlined->length )
                        .arg( "what", std::string{ what } ) );
        inlined.reset();
      }
    };
    if ( storage.has_value() && storage->keyword == Keyword::TYPEDEF )
    {
      withoutBody( "a function type" );
      // The one typedef the subset has names a function type, and the parser
      // reads it as the prototype it looks like.
      if ( std::optional<FunctionDefinition> type = parseFunctionDefinition( storage, inlined ); type.has_value() )
      {
        type->attributes = std::move( *attributes );
        unit.declarations.emplace_back( std::move( *type ) );
      }
      continue;
    }
    if ( atKeyword( Keyword::VOID_TYPE ) || ( !atQualifier() && functionAhead() ) )
    {
      if ( std::optional<FunctionDefinition> function = parseFunctionDefinition( storage, inlined );
           function.has_value() )
      {
        function->attributes = std::move( *attributes );
        unit.declarations.emplace_back( std::move( *function ) );
      }
      continue;
    }
    if ( attributed && ( atKeyword( Keyword::STRUCT ) || atKeyword( Keyword::UNION ) || atKeyword( Keyword::ENUM ) ) )
    {
      fail( "a declaration or a function" );
      recover( true );
      continue;
    }
    if ( !storage.has_value() && ( atKeyword( Keyword::STRUCT ) || atKeyword( Keyword::UNION ) ) )
    {
      withoutBody( "a `struct`" );
      if ( std::optional<StructSpecifier> aggregate = parseStructSpecifier(); aggregate.has_value() )
      {
        unit.declarations.emplace_back( std::move( *aggregate ) );
      }
      continue;
    }
    if ( !storage.has_value() && atKeyword( Keyword::ENUM ) )
    {
      withoutBody( "an `enum`" );
      if ( std::optional<EnumSpecifier> enumeration = parseEnumSpecifier(); enumeration.has_value() )
      {
        unit.declarations.emplace_back( std::move( *enumeration ) );
      }
      continue;
    }
    // A type is a keyword, or the name of an `enum struct` written as C++
    // writes one — see docs/decisions/0078-switch-over-an-enum-struct.md. A
    // `(` after the name makes it a function's.
    if ( functionAhead() )
    {
      if ( std::optional<FunctionDefinition> function = parseFunctionDefinition( storage, inlined );
           function.has_value() )
      {
        function->attributes = std::move( *attributes );
        unit.declarations.emplace_back( std::move( *function ) );
      }
      continue;
    }
    // `auto` is read here and refused where the check knows a file-scope
    // object is given a constant — see
    // docs/decisions/0161-auto-takes-the-type-of-the-value.md.
    if ( isType( peek().keyword ) || at( TokenKind::IDENTIFIER ) || atQualifier() || atKeyword( Keyword::AUTO ) )
    {
      withoutBody( "an object" );
      if ( std::optional<Declaration> declaration = parseDeclaration( storage ); declaration.has_value() )
      {
        declaration->attributes = std::move( *attributes );
        unit.declarations.emplace_back( std::move( *declaration ) );
      }
      continue;
    }
    // One spelling of a type, so `int` and `char` are not a second one — see
    // docs/decisions/0076-arithmetic-in-the-subset.md.
    if ( atKeyword( Keyword::INT ) || atKeyword( Keyword::CHAR ) )
    {
      Token const found = peek();
      mSink->add( diagnostic( diag::DiagnosticId::C_TYPE_SPELLED_OTHERWISE )
                      .at( found.location, found.length )
                      .arg( "found", std::string{ mSources->textOf( found.span() ) } )
                      .arg( "spelling", std::string{ found.keyword == Keyword::INT ? "i16" : "u8" } ) );
      recover( true );
      continue;
    }
    fail( storage.has_value() ? "`void` or a type" : "a declaration" );
    recover( true );
  }
  return unit;
}

bool Parser::declarationAhead() const
{
  Token const& first = peek();
  Token const& second = peekAhead( 1 );
  Token const& third = peekAhead( 2 );
  // A name followed by a name, a qualifier, or a `*` and one of those: the
  // shape a declaration of locals is told by where a statement may begin one.
  bool const named =
      first.kind == TokenKind::IDENTIFIER &&
      ( second.kind == TokenKind::IDENTIFIER || isQualifier( second ) ||
        ( second.kind == TokenKind::STAR && ( third.kind == TokenKind::IDENTIFIER || isQualifier( third ) ) ) );
  return isType( first.keyword ) || named || first.keyword == Keyword::STATIC || isQualifier( first ) ||
         first.keyword == Keyword::AUTO;
}

bool Parser::initialiserAhead() const
{
  // A `;` before the `)` that closes the clause, nothing nested holding it:
  // `if (u8 c = f(); c != 0)`.
  std::size_t depth = 1;
  for ( std::size_t ahead = 0;; ++ahead )
  {
    switch ( peekAhead( ahead ).kind )
    {
    case TokenKind::LEFT_PAREN:
    case TokenKind::LEFT_BRACKET:
      ++depth;
      break;
    case TokenKind::RIGHT_BRACKET:
      depth -= depth > 0 ? 1 : 0;
      break;
    case TokenKind::RIGHT_PAREN:
      if ( depth <= 1 )
      {
        return false;
      }
      --depth;
      break;
    case TokenKind::SEMICOLON:
      if ( depth == 1 )
      {
        return true;
      }
      break;
    case TokenKind::LEFT_BRACE:
    case TokenKind::END_OF_FILE:
      return false;
    default:
      break;
    }
  }
}

bool Parser::functionAhead() const
{
  std::size_t ahead = 0;
  while ( isQualifier( peekAhead( ahead ) ) )
  {
    ++ahead;
  }
  Token const& type = peekAhead( ahead++ );
  bool const keywordType = type.kind == TokenKind::KEYWORD && isType( type.keyword );
  if ( !keywordType && type.kind != TokenKind::IDENTIFIER )
  {
    return false;
  }
  while ( isQualifier( peekAhead( ahead ) ) )
  {
    ++ahead;
  }
  if ( peekAhead( ahead ).kind == TokenKind::STAR )
  {
    ++ahead;
  }
  return peekAhead( ahead ).kind == TokenKind::IDENTIFIER && peekAhead( ahead + 1 ).kind == TokenKind::LEFT_PAREN;
}

void Parser::matchQualifiers( Qualifiers& into )
{
  while ( atQualifier() )
  {
    bool& written = atKeyword( Keyword::CONST_QUALIFIER ) ? into.isConst : into.isVolatile;
    if ( written )
    {
      return;
    }
    written = true;
    advance();
  }
}

std::optional<FunctionDefinition> Parser::parseFunctionDefinition( std::optional<Token> storage,
                                                                   std::optional<Token> inlined )
{
  diag::SourceSpan first = peek().span();
  // After `extern`, a declaration of a Proc of the assembler: no body, and a
  // parameter's name may be left out. After `typedef`, a function type, which
  // has a body no more than a prototype does and names every parameter, since
  // those names are the Temporaries its members read — see
  // docs/decisions/0065-handlers.md.
  bool const named = storage.has_value() && storage->keyword == Keyword::TYPEDEF;
  bool const prototype = ( storage.has_value() && storage->keyword == Keyword::EXTERN ) || named;
  if ( prototype && inlined.has_value() )
  {
    mSink->add( diagnostic( diag::DiagnosticId::C_INLINE_WITHOUT_BODY )
                    .at( inlined->location, inlined->length )
                    .arg( "what", std::string{ named ? "a function type" : "a `.proc` of the assembler" } ) );
    inlined.reset();
  }
  Qualifiers result;
  matchQualifiers( result );
  Token const type = advance();
  if ( storage.has_value() )
  {
    first = storage->span();
  }
  bool resultPointer = false;
  if ( type.keyword != Keyword::VOID_TYPE )
  {
    matchQualifiers( result );
    resultPointer = match( TokenKind::STAR );
  }
  if ( !at( TokenKind::IDENTIFIER ) )
  {
    fail( "a name" );
    recover( true );
    return std::nullopt;
  }
  Token const name = advance();

  if ( !match( TokenKind::LEFT_PAREN ) )
  {
    fail( "`(`" );
    recover( true );
    return std::nullopt;
  }
  // C23 reads `()` as `(void)`, so both are a list of no parameters.
  std::vector<Parameter> parameters;
  if ( atKeyword( Keyword::VOID_TYPE ) && peekAhead( 1 ).kind == TokenKind::RIGHT_PAREN )
  {
    advance();
  }
  else if ( !at( TokenKind::RIGHT_PAREN ) )
  {
    for ( ;; )
    {
      if ( atKeyword( Keyword::INT ) || atKeyword( Keyword::CHAR ) )
      {
        Token const found = peek();
        mSink->add( diagnostic( diag::DiagnosticId::C_TYPE_SPELLED_OTHERWISE )
                        .at( found.location, found.length )
                        .arg( "found", std::string{ mSources->textOf( found.span() ) } )
                        .arg( "spelling", std::string{ found.keyword == Keyword::INT ? "i16" : "u8" } ) );
        recover( true );
        return std::nullopt;
      }
      Qualifiers pointee;
      matchQualifiers( pointee );
      if ( !isType( peek().keyword ) && !at( TokenKind::IDENTIFIER ) )
      {
        fail( "a type" );
        recover( true );
        return std::nullopt;
      }
      Token const parameterType = advance();
      matchQualifiers( pointee );
      bool const isPointer = match( TokenKind::STAR );
      // A parameter of a body is named: nothing could read one that is not.
      bool const unnamed = prototype && !named && ( at( TokenKind::COMMA ) || at( TokenKind::RIGHT_PAREN ) );
      if ( !unnamed && !at( TokenKind::IDENTIFIER ) )
      {
        fail( "a name" );
        recover( true );
        return std::nullopt;
      }
      Parameter parameter{ .type = parameterType,
                           .name = unnamed ? parameterType : advance(),
                           .isPointer = isPointer,
                           .pointeeConst = pointee.isConst,
                           .pointeeVolatile = pointee.isVolatile,
                           .size = std::nullopt,
                           .isNamed = !unnamed };
      // An array parameter is a pointer, as C has it.
      if ( !isPointer && at( TokenKind::LEFT_BRACKET ) )
      {
        Token const open = advance();
        parameter.isPointer = true;
        if ( !at( TokenKind::RIGHT_BRACKET ) )
        {
          ExpressionPtr const size = parseBinary( LOWEST_PRECEDENCE );
          if ( size == nullptr )
          {
            recover( true );
            return std::nullopt;
          }
          parameter.size = spanning( open.span(), size->span );
        }
        if ( !match( TokenKind::RIGHT_BRACKET ) )
        {
          fail( "`]`" );
          recover( true );
          return std::nullopt;
        }
      }
      parameters.push_back( parameter );
      if ( !match( TokenKind::COMMA ) )
      {
        break;
      }
    }
  }
  if ( !match( TokenKind::RIGHT_PAREN ) )
  {
    fail( parameters.empty() ? "`)`" : "`,` or `)`" );
    recover( true );
    return std::nullopt;
  }

  if ( prototype )
  {
    Token const end = peek();
    if ( !match( TokenKind::SEMICOLON ) )
    {
      fail( "`;`" );
      recover( true );
      return std::nullopt;
    }
    return FunctionDefinition{ .name = name,
                               .span = spanning( first, end.span() ),
                               .body = Statement{ .kind = StatementKind::NULL_STATEMENT,
                                                  .span = end.span(),
                                                  .expression = {},
                                                  .initial = {},
                                                  .step = {},
                                                  .items = {},
                                                  .declaration = {} },
                               .attributes = {},
                               .isStatic = false,
                               .result = type.keyword == Keyword::VOID_TYPE ? std::nullopt : std::optional{ type },
                               .resultPointer = resultPointer,
                               .resultPointeeConst = result.isConst,
                               .resultPointeeVolatile = result.isVolatile,
                               .parameters = std::move( parameters ),
                               .isExtern = !named,
                               .isTypedef = named };
  }
  if ( !at( TokenKind::LEFT_BRACE ) )
  {
    fail( "`{`" );
    recover( true );
    return std::nullopt;
  }
  std::optional<Statement> body = parseCompoundStatement();
  if ( !body.has_value() )
  {
    return std::nullopt;
  }

  diag::SourceSpan const span = spanning( first, body->span );
  return FunctionDefinition{ .name = name,
                             .span = span,
                             .body = std::move( *body ),
                             .isStatic = storage.has_value() && storage->keyword == Keyword::STATIC,
                             .isInline = inlined.has_value(),
                             .inlineSpan = inlined.has_value() ? inlined->span() : diag::SourceSpan{},
                             .result = type.keyword == Keyword::VOID_TYPE ? std::nullopt : std::optional{ type },
                             .resultPointer = resultPointer,
                             .resultPointeeConst = result.isConst,
                             .resultPointeeVolatile = result.isVolatile,
                             .parameters = std::move( parameters ) };
}

std::optional<Declaration> Parser::parseDeclaration( std::optional<Token> storage )
{
  std::optional<Token> qualifier;
  if ( atQualifier() )
  {
    qualifier = peek();
  }
  // `const` and `volatile` stand before the type or after it, each once.
  Qualifiers qualifiers;
  matchQualifiers( qualifiers );
  // `auto` stands where a type does, and the value it is given says which —
  // see docs/decisions/0161-auto-takes-the-type-of-the-value.md.
  if ( !isType( peek().keyword ) && !at( TokenKind::IDENTIFIER ) && !atKeyword( Keyword::AUTO ) )
  {
    fail( "a type" );
    recover( true );
    return std::nullopt;
  }
  Token const type = advance();
  diag::SourceSpan first = type.span();
  if ( qualifier.has_value() )
  {
    first = qualifier->span();
  }
  if ( storage.has_value() )
  {
    first = storage->span();
  }
  matchQualifiers( qualifiers );
  std::vector<Token> declarators;
  std::vector<ExpressionPtr> initialisers;
  std::vector<DeclaratorShape> arrays;
  for ( ;; )
  {
    // `*` belongs to the declarator, as C has it, and `const` or `volatile`
    // after it makes the pointer itself one.
    DeclaratorShape array;
    array.isPointer = match( TokenKind::STAR );
    if ( array.isPointer )
    {
      Qualifiers pointer;
      matchQualifiers( pointer );
      array.isConstPointer = pointer.isConst;
      array.isVolatilePointer = pointer.isVolatile;
    }
    if ( !at( TokenKind::IDENTIFIER ) )
    {
      fail( "a name" );
      recover( true );
      return std::nullopt;
    }
    declarators.push_back( advance() );

    // One dimension, its size perhaps left to the list.
    if ( match( TokenKind::LEFT_BRACKET ) )
    {
      array.isArray = true;
      if ( !at( TokenKind::RIGHT_BRACKET ) )
      {
        array.size = parseBinary( LOWEST_PRECEDENCE );
        if ( array.size == nullptr )
        {
          recover( true );
          return std::nullopt;
        }
      }
      if ( !match( TokenKind::RIGHT_BRACKET ) )
      {
        fail( "`]`" );
        recover( true );
        return std::nullopt;
      }
    }

    // A value where one is written; whether an object may be given one where
    // it stands, and whether it is given a list, is the compiler's question.
    ExpressionPtr value;
    if ( match( TokenKind::EQUAL ) )
    {
      if ( at( TokenKind::LEFT_BRACE ) )
      {
        std::size_t const listStart = mIndex;
        Token const open = advance();
        array.hasList = true;
        // A list that fails is skipped from its `{` past its `}`, and the
        // declaration then to its `;`, as one finding.
        auto const skipList = [this, listStart]
        {
          mIndex = listStart;
          recover( true );
        };
        while ( !at( TokenKind::RIGHT_BRACE ) )
        {
          ExpressionPtr element = parseListElement();
          if ( element == nullptr )
          {
            skipList();
            return std::nullopt;
          }
          array.list.push_back( std::move( element ) );
          if ( !match( TokenKind::COMMA ) )
          {
            break;
          }
        }
        Token const close = peek();
        if ( !match( TokenKind::RIGHT_BRACE ) )
        {
          fail( "`,` or `}`" );
          skipList();
          return std::nullopt;
        }
        array.listSpan = spanning( open.span(), close.span() );
      }
      else if ( array.isArray && at( TokenKind::STRING_LITERAL ) )
      {
        // An array a string literal gives is given its characters and its end
        // as a list, which C drops where the array is exactly as long.
        ExpressionPtr literal = parseStringLiteral();
        array.hasList = true;
        array.fromLiteral = true;
        array.listSpan = literal->span;
        array.list = std::move( literal->arguments );
      }
      else
      {
        // A value given where a name is declared is an expression, `? :`
        // included; a list's elements are constants and take no condition.
        value = parseConditional();
        if ( value == nullptr )
        {
          recover( true );
          return std::nullopt;
        }
      }
    }
    initialisers.push_back( std::move( value ) );
    arrays.push_back( std::move( array ) );

    if ( match( TokenKind::COMMA ) )
    {
      continue;
    }
    if ( at( TokenKind::SEMICOLON ) )
    {
      break;
    }
    fail( "`,` or `;`" );
    recover( true );
    return std::nullopt;
  }
  Token const end = advance();
  return Declaration{ .type = type,
                      .declarators = std::move( declarators ),
                      .initialisers = std::move( initialisers ),
                      .shapes = std::move( arrays ),
                      .span = spanning( first, end.span() ),
                      .isStatic = storage.has_value() && storage->keyword == Keyword::STATIC,
                      .attributes = {},
                      .isConst = qualifiers.isConst,
                      .isVolatile = qualifiers.isVolatile,
                      .isExtern = storage.has_value() && storage->keyword == Keyword::EXTERN };
}

ExpressionPtr Parser::parseStringLiteral()
{
  Token const first = advance();
  std::string_view const firstText = mSources->textOf( first.span() );
  std::string_view const prefix = firstText.substr( 0, firstText.find( '"' ) );
  ExpressionPtr node = makeExpression( ExpressionKind::STRING_LITERAL, first, first.span() );
  Token last = first;
  for ( Token piece = first;; piece = advance() )
  {
    std::string_view const text = mSources->textOf( piece.span() );
    std::string_view const piecePrefix = text.substr( 0, text.find( '"' ) );
    // Adjacent literals join under one prefix, and no prefix is one — see
    // docs/decisions/0095-literals-and-the-runtime.md.
    if ( piecePrefix != prefix )
    {
      mSink->add(
          diagnostic( diag::DiagnosticId::C_LITERALS_OF_TWO_PREFIXES )
              .at( piece.location, piece.length )
              .arg( "first", std::string{ prefix.empty() ? "no prefix" : "`" + std::string{ prefix } + "`" } )
              .arg( "second",
                    std::string{ piecePrefix.empty() ? "no prefix" : "`" + std::string{ piecePrefix } + "`" } ) );
    }
    std::string_view body = text.substr( piecePrefix.size() + 1 );
    body.remove_suffix( 1 );
    for ( std::size_t at = 0; at < body.size(); )
    {
      std::size_t length = 1;
      if ( body[at] == '\\' )
      {
        length = 2;
      }
      else if ( ( static_cast<unsigned char>( body[at] ) & 0x80U ) != 0 )
      {
        length = std::max<std::uint32_t>( 1, syntax::utf8LengthAt( body, static_cast<std::uint32_t>( at ) ) );
      }
      node->arguments.push_back( literalCharacter( piece, prefix, body.substr( at, length ) ) );
      at += length;
    }
    last = piece;
    if ( !at( TokenKind::STRING_LITERAL ) )
    {
      break;
    }
  }
  node->arguments.push_back( literalCharacter( last, prefix, "\\0" ) );
  node->span = spanning( first.span(), last.span() );
  return node;
}

ExpressionPtr Parser::parseCharacterConstant()
{
  Token const token = advance();
  std::string_view const text = mSources->textOf( token.span() );
  std::size_t const open = text.find( '\'' );
  std::string_view body = text.substr( open + 1 );
  body.remove_suffix( 1 );
  ExpressionPtr node = makeExpression( ExpressionKind::CHARACTER_CONSTANT, token, token.span() );
  node->value = body.size() == 2 && body[0] == '\\' ? escapedValue( body[1] ) : static_cast<unsigned char>( body[0] );
  return node;
}

std::optional<Statement> Parser::parseCompoundStatement()
{
  Token const open = advance();
  if ( mBlocks == MAX_BLOCK_DEPTH )
  {
    refuseDepth( open, "blocks", MAX_BLOCK_DEPTH );
    // Skipped without being read, since reading it is the descent the limit
    // refuses; whatever is wrong inside says nothing more.
    std::uint32_t depth = 1;
    while ( !at( TokenKind::END_OF_FILE ) )
    {
      TokenKind const kind = advance().kind;
      if ( kind == TokenKind::LEFT_BRACE )
      {
        ++depth;
      }
      else if ( kind == TokenKind::RIGHT_BRACE && --depth == 0 )
      {
        break;
      }
    }
    return std::nullopt;
  }

  Statement block{ .kind = StatementKind::COMPOUND,
                   .span = {},
                   .expression = {},
                   .initial = {},
                   .step = {},
                   .items = {},
                   .declaration = {} };
  ++mBlocks;
  while ( !at( TokenKind::RIGHT_BRACE ) && !at( TokenKind::END_OF_FILE ) )
  {
    if ( std::optional<Statement> item = parseStatement(); item.has_value() )
    {
      block.items.push_back( std::move( *item ) );
    }
  }
  --mBlocks;

  Token const close = peek();
  if ( !match( TokenKind::RIGHT_BRACE ) )
  {
    fail( "`}`" );
  }
  block.span = spanning( open.span(), close.span() );
  return block;
}

std::optional<Statement> Parser::parseStatement()
{
  // Attributes stand before a declaration of locals, before a block —
  // `[[with(...)]]`, see docs/decisions/0096-panes-in-c.md — and before a
  // `return`, which `[[transition(PHASE)]]` turns into entering a Phase; see
  // docs/decisions/0064-phase-in-c.md.
  if ( atAttributes() )
  {
    std::optional<std::vector<Attribute>> attributes = parseAttributes();
    if ( !attributes.has_value() )
    {
      return std::nullopt;
    }
    if ( at( TokenKind::LEFT_BRACE ) )
    {
      std::optional<Statement> block = parseCompoundStatement();
      if ( block.has_value() && !attributes->empty() )
      {
        // The block begins at its attributes, whose line a finding on what
        // it became names.
        block->span = spanning( attributes->front().span, block->span );
        block->attributes = std::move( *attributes );
      }
      return block;
    }
    Token const next = peek();
    bool const returning = next.keyword == Keyword::RETURN;
    bool const declaration = isType( next.keyword ) || next.kind == TokenKind::IDENTIFIER ||
                             next.keyword == Keyword::STATIC || isQualifier( next ) || next.keyword == Keyword::AUTO;
    if ( !declaration && !returning )
    {
      fail( "a declaration, a block or `return`" );
      recover( false );
      return std::nullopt;
    }
    std::optional<Statement> statement = parseStatement();
    if ( statement.has_value() && statement->kind == StatementKind::DECLARATION )
    {
      statement->declaration->attributes = std::move( *attributes );
    }
    else if ( statement.has_value() && statement->kind == StatementKind::RETURN )
    {
      // The `return` begins at its attributes, whose line the text's own
      // finding about the Phase names.
      statement->span = spanning( attributes->front().span, statement->span );
      statement->attributes = std::move( *attributes );
    }
    else if ( statement.has_value() )
    {
      mSink->add( diagnostic( diag::DiagnosticId::C_EXPECTED )
                      .at( next.location, next.length )
                      .arg( "expected", std::string{ "a declaration or `return`" } )
                      .arg( "found", "`" + std::string{ mSources->textOf( next.span() ) } + "`" ) );
    }
    return statement;
  }
  Token const first = peek();
  if ( at( TokenKind::LEFT_BRACE ) )
  {
    return parseCompoundStatement();
  }
  if ( match( TokenKind::SEMICOLON ) )
  {
    return Statement{ .kind = StatementKind::NULL_STATEMENT,
                      .span = first.span(),
                      .expression = {},
                      .initial = {},
                      .step = {},
                      .items = {},
                      .declaration = {} };
  }

  // A declaration of locals, told from a statement by the name, the `const`
  // or the `*` and a name that follows a type's: `Light c;` declares and
  // `c = 1;` assigns. `a * b;` would be an expression statement that neither
  // assigns nor calls, which the subset refuses anyway.
  Token const& second = peekAhead( 1 );
  Token const& third = peekAhead( 2 );
  bool const named =
      first.kind == TokenKind::IDENTIFIER &&
      ( second.kind == TokenKind::IDENTIFIER || isQualifier( second ) ||
        ( second.kind == TokenKind::STAR && ( third.kind == TokenKind::IDENTIFIER || isQualifier( third ) ) ) );
  if ( isType( first.keyword ) || named || first.keyword == Keyword::STATIC || isQualifier( first ) ||
       first.keyword == Keyword::AUTO )
  {
    std::optional<Token> const storage =
        atKeyword( Keyword::STATIC ) ? std::optional<Token>{ advance() } : std::optional<Token>{};
    std::optional<Declaration> declaration = parseDeclaration( storage );
    if ( !declaration.has_value() )
    {
      return std::nullopt;
    }
    diag::SourceSpan const span = declaration->span;
    return Statement{ .kind = StatementKind::DECLARATION,
                      .span = span,
                      .expression = {},
                      .initial = {},
                      .step = {},
                      .items = {},
                      .declaration = std::make_unique<Declaration>( std::move( *declaration ) ) };
  }
  if ( atKeyword( Keyword::RETURN ) && peekAhead( 1 ).kind != TokenKind::SEMICOLON )
  {
    advance();
    ExpressionPtr value = parseAssignment();
    if ( value == nullptr )
    {
      recover( false );
      return std::nullopt;
    }
    Token const end = peek();
    if ( !match( TokenKind::SEMICOLON ) )
    {
      fail( "`;`" );
      recover( false );
      return std::nullopt;
    }
    return Statement{ .kind = StatementKind::RETURN,
                      .span = spanning( first.span(), end.span() ),
                      .expression = std::move( value ),
                      .initial = {},
                      .step = {},
                      .items = {},
                      .declaration = {} };
  }
  if ( atKeyword( Keyword::RETURN ) || atKeyword( Keyword::BREAK ) || atKeyword( Keyword::CONTINUE ) )
  {
    advance();
    Token const end = peek();
    if ( !match( TokenKind::SEMICOLON ) )
    {
      fail( "`;`" );
      recover( false );
      return std::nullopt;
    }
    StatementKind kind = StatementKind::RETURN;
    if ( first.keyword == Keyword::BREAK )
    {
      kind = StatementKind::BREAK;
    }
    else if ( first.keyword == Keyword::CONTINUE )
    {
      kind = StatementKind::CONTINUE;
    }
    return Statement{ .kind = kind,
                      .span = spanning( first.span(), end.span() ),
                      .expression = {},
                      .initial = {},
                      .step = {},
                      .items = {},
                      .declaration = {} };
  }
  if ( atKeyword( Keyword::IF ) || atKeyword( Keyword::WHILE ) || atKeyword( Keyword::DO ) ||
       atKeyword( Keyword::FOR ) || atKeyword( Keyword::SWITCH ) )
  {
    // A control statement nests as a block does, and is counted with them,
    // since reading the one it governs is a descent too.
    if ( mBlocks == MAX_BLOCK_DEPTH )
    {
      refuseDepth( first, "blocks and statements", MAX_BLOCK_DEPTH );
      recover( false );
      return std::nullopt;
    }
    ++mBlocks;
    std::optional<Statement> control = parseControl();
    --mBlocks;
    return control;
  }

  if ( !canBeginExpression( first ) )
  {
    fail( "a statement" );
    recover( false );
    return std::nullopt;
  }
  ExpressionPtr expression = parseAssignment();
  if ( expression == nullptr )
  {
    recover( false );
    return std::nullopt;
  }
  Token const end = peek();
  if ( !match( TokenKind::SEMICOLON ) )
  {
    fail( "`;`" );
    recover( false );
    return std::nullopt;
  }
  return Statement{ .kind = StatementKind::EXPRESSION,
                    .span = spanning( first.span(), end.span() ),
                    .expression = std::move( expression ),
                    .initial = {},
                    .step = {},
                    .items = {},
                    .declaration = {} };
}

std::optional<Statement> Parser::parseControl()
{
  Token const first = advance();
  Statement control{ .kind = StatementKind::IF,
                     .span = first.span(),
                     .expression = {},
                     .initial = {},
                     .step = {},
                     .items = {},
                     .declaration = {} };

  if ( first.keyword == Keyword::SWITCH )
  {
    control.kind = StatementKind::SWITCH;
    control.expression = parseCondition( &control );
    if ( control.expression == nullptr || !at( TokenKind::LEFT_BRACE ) )
    {
      if ( control.expression != nullptr )
      {
        fail( "`{`" );
      }
      recover( false );
      return std::nullopt;
    }
    parseSwitchBody( control );
    control.span = spanning( first.span(), control.span );
    return control;
  }

  if ( first.keyword == Keyword::DO )
  {
    control.kind = StatementKind::DO;
    control.items.push_back( parseBody() );
    if ( !atKeyword( Keyword::WHILE ) )
    {
      fail( "`while`" );
      recover( false );
      return std::nullopt;
    }
    advance();
    control.expression = parseCondition();
    Token const end = peek();
    if ( control.expression == nullptr || !match( TokenKind::SEMICOLON ) )
    {
      if ( control.expression != nullptr )
      {
        fail( "`;`" );
      }
      recover( false );
      return std::nullopt;
    }
    control.span = spanning( first.span(), end.span() );
    return control;
  }

  if ( first.keyword == Keyword::FOR )
  {
    control.kind = StatementKind::FOR;
    if ( !match( TokenKind::LEFT_PAREN ) )
    {
      fail( "`(`" );
      recover( false );
      return std::nullopt;
    }
    // The first clause may declare locals of its own, in scope in the loop and
    // nowhere else — see
    // docs/decisions/0160-a-declaration-where-a-statement-begins-one.md.
    if ( declarationAhead() )
    {
      std::optional<Token> const storage =
          atKeyword( Keyword::STATIC ) ? std::optional<Token>{ advance() } : std::optional<Token>{};
      std::optional<Declaration> declared = parseDeclaration( storage );
      if ( !declared.has_value() )
      {
        recover( false );
        return std::nullopt;
      }
      control.declaration = std::make_unique<Declaration>( std::move( *declared ) );
    }

    // Each clause may be left out, and each is ended by what follows it.
    auto const clause = [this]( ExpressionPtr& into, TokenKind end, std::string_view expected )
    {
      if ( !at( end ) )
      {
        into = parseAssignment();
        if ( into == nullptr )
        {
          return false;
        }
      }
      if ( !match( end ) )
      {
        fail( expected );
        return false;
      }
      return true;
    };
    if ( ( control.declaration == nullptr && !clause( control.initial, TokenKind::SEMICOLON, "`;`" ) ) ||
         !clause( control.expression, TokenKind::SEMICOLON, "`;`" ) ||
         !clause( control.step, TokenKind::RIGHT_PAREN, "`)`" ) )
    {
      recover( false );
      return std::nullopt;
    }
  }
  else
  {
    control.kind = first.keyword == Keyword::IF ? StatementKind::IF : StatementKind::WHILE;
    control.expression = parseCondition( control.kind == StatementKind::IF ? &control : nullptr );
    if ( control.expression == nullptr )
    {
      recover( false );
      return std::nullopt;
    }
  }

  control.items.push_back( parseBody() );
  if ( control.kind == StatementKind::IF && atKeyword( Keyword::ELSE ) )
  {
    advance();
    control.items.push_back( parseBody() );
  }
  control.span = spanning( first.span(), control.items.back().span );
  return control;
}

ExpressionPtr Parser::parseCondition( Statement* initialised )
{
  if ( !match( TokenKind::LEFT_PAREN ) )
  {
    fail( "`(`" );
    return nullptr;
  }

  // `if (u8 c = f(); c != 0)` and `switch (u8 c = f(); c)`: what stands before
  // the `;` is a statement of its own, in scope in the whole `if` or `switch`
  // — see docs/decisions/0160-a-declaration-where-a-statement-begins-one.md.
  if ( initialised != nullptr && initialiserAhead() )
  {
    if ( declarationAhead() )
    {
      std::optional<Token> const storage =
          atKeyword( Keyword::STATIC ) ? std::optional<Token>{ advance() } : std::optional<Token>{};
      std::optional<Declaration> declared = parseDeclaration( storage );
      if ( !declared.has_value() )
      {
        return nullptr;
      }
      initialised->declaration = std::make_unique<Declaration>( std::move( *declared ) );
    }
    else
    {
      initialised->initial = parseAssignment();
      if ( initialised->initial == nullptr || !match( TokenKind::SEMICOLON ) )
      {
        fail( "`;`" );
        return nullptr;
      }
    }
  }
  ExpressionPtr condition = parseAssignment();
  if ( condition == nullptr )
  {
    return nullptr;
  }
  if ( !match( TokenKind::RIGHT_PAREN ) )
  {
    fail( "`)`" );
    return nullptr;
  }
  return condition;
}

std::optional<EnumSpecifier> Parser::parseEnumSpecifier()
{
  Token const first = advance();

  // Inside the braces, a failure skips past the closing one and the `;` after
  // it, which is where the declaration ends.
  auto const abandon = [this]( std::string_view expected )
  {
    fail( expected );
    while ( !at( TokenKind::END_OF_FILE ) && !at( TokenKind::RIGHT_BRACE ) && !at( TokenKind::SEMICOLON ) )
    {
      advance();
    }
    match( TokenKind::RIGHT_BRACE );
    match( TokenKind::SEMICOLON );
    return std::nullopt;
  };

  // `enum struct NAME` keeps its enumerators to the type's name, and `enum
  // NAME` gives them to the file as well — see
  // docs/decisions/0162-an-enum-without-struct.md.
  bool const scoped = atKeyword( Keyword::STRUCT );
  if ( scoped )
  {
    advance();
  }
  if ( !at( TokenKind::IDENTIFIER ) )
  {
    fail( "a name" );
    recover( true );
    return std::nullopt;
  }
  Token const name = advance();
  if ( !match( TokenKind::LEFT_BRACE ) )
  {
    fail( "`{`" );
    recover( true );
    return std::nullopt;
  }

  std::vector<Token> enumerators;
  for ( ;; )
  {
    if ( !at( TokenKind::IDENTIFIER ) )
    {
      return abandon( "an enumerator" );
    }
    enumerators.push_back( advance() );
    // A comma may end the list, as C allows.
    if ( match( TokenKind::COMMA ) && !at( TokenKind::RIGHT_BRACE ) )
    {
      continue;
    }
    if ( at( TokenKind::RIGHT_BRACE ) )
    {
      break;
    }
    return abandon( "`,` or `}`" );
  }
  advance();
  Token const end = peek();
  if ( !match( TokenKind::SEMICOLON ) )
  {
    fail( "`;`" );
    recover( true );
    return std::nullopt;
  }
  return EnumSpecifier{ .name = name,
                        .enumerators = std::move( enumerators ),
                        .span = spanning( first.span(), end.span() ),
                        .isScoped = scoped };
}

bool Parser::atAttributes() const
{
  return at( TokenKind::LEFT_BRACKET ) && peekAhead( 1 ).kind == TokenKind::LEFT_BRACKET;
}

std::optional<std::vector<Attribute>> Parser::parseAttributes()
{
  std::vector<Attribute> attributes;
  while ( atAttributes() )
  {
    advance();
    advance();
    while ( !at( TokenKind::RIGHT_BRACKET ) )
    {
      Token const first = peek();
      if ( !match( TokenKind::IDENTIFIER ) )
      {
        fail( "an attribute" );
        recover( true );
        return std::nullopt;
      }
      Attribute attribute{ .name = first, .prefix = std::nullopt, .hasArguments = false, .span = first.span() };
      if ( match( TokenKind::COLON_COLON ) )
      {
        Token const name = peek();
        if ( !match( TokenKind::IDENTIFIER ) )
        {
          fail( "an attribute" );
          recover( true );
          return std::nullopt;
        }
        attribute.prefix = first;
        attribute.name = name;
        attribute.span = spanning( first.span(), name.span() );
      }
      // Arguments are balanced tokens, which an attribute reads as it will.
      if ( at( TokenKind::LEFT_PAREN ) )
      {
        attribute.hasArguments = true;
        std::uint32_t depth = 0;
        do
        {
          if ( at( TokenKind::END_OF_FILE ) )
          {
            fail( "`)`" );
            return std::nullopt;
          }
          depth += at( TokenKind::LEFT_PAREN ) ? 1U : 0U;
          depth -= at( TokenKind::RIGHT_PAREN ) ? 1U : 0U;
          Token const token = advance();
          attribute.span = spanning( attribute.span, token.span() );
          attribute.arguments.push_back( token );
        } while ( depth > 0 );
        // The tokens between the parentheses, and not those.
        attribute.arguments.erase( attribute.arguments.begin() );
        attribute.arguments.pop_back();
      }
      attributes.push_back( attribute );
      if ( !match( TokenKind::COMMA ) )
      {
        break;
      }
    }
    if ( !match( TokenKind::RIGHT_BRACKET ) || !match( TokenKind::RIGHT_BRACKET ) )
    {
      fail( "`]]`" );
      recover( true );
      return std::nullopt;
    }
  }
  return attributes;
}

std::optional<StructSpecifier> Parser::parseStructSpecifier()
{
  Token const keyword = advance();
  if ( !at( TokenKind::IDENTIFIER ) )
  {
    fail( "a name" );
    recover( true );
    return std::nullopt;
  }
  Token const name = advance();
  if ( !match( TokenKind::LEFT_BRACE ) )
  {
    fail( "`{`" );
    recover( true );
    return std::nullopt;
  }
  StructSpecifier aggregate{
    .name = name, .isUnion = keyword.keyword == Keyword::UNION, .members = {}, .span = keyword.span()
  };
  while ( !at( TokenKind::RIGHT_BRACE ) && !at( TokenKind::END_OF_FILE ) )
  {
    if ( !isType( peek().keyword ) && !at( TokenKind::IDENTIFIER ) && !atQualifier() )
    {
      fail( "a member" );
      recover( true );
      return std::nullopt;
    }
    if ( std::optional<Declaration> member = parseDeclaration( std::nullopt ); member.has_value() )
    {
      aggregate.members.push_back( std::move( *member ) );
    }
  }
  if ( !match( TokenKind::RIGHT_BRACE ) )
  {
    fail( "`}`" );
    return std::nullopt;
  }
  Token const end = peek();
  if ( !match( TokenKind::SEMICOLON ) )
  {
    fail( "`;`" );
    recover( true );
    return std::nullopt;
  }
  aggregate.span = spanning( keyword.span(), end.span() );
  return aggregate;
}

ExpressionPtr Parser::parseListElement()
{
  if ( !at( TokenKind::LEFT_BRACE ) )
  {
    return parseBinary( LOWEST_PRECEDENCE );
  }
  Token const open = advance();
  if ( mParentheses == MAX_EXPRESSION_DEPTH )
  {
    refuseDepth( open, "parentheses", MAX_EXPRESSION_DEPTH );
    return nullptr;
  }
  ExpressionPtr list = makeExpression( ExpressionKind::LIST, open, open.span() );
  ++mParentheses;
  while ( !at( TokenKind::RIGHT_BRACE ) )
  {
    ExpressionPtr element = parseListElement();
    if ( element == nullptr )
    {
      --mParentheses;
      return nullptr;
    }
    list->depth = std::max( list->depth, element->depth + 1 );
    list->arguments.push_back( std::move( element ) );
    if ( !match( TokenKind::COMMA ) )
    {
      break;
    }
  }
  --mParentheses;
  Token const close = peek();
  if ( !match( TokenKind::RIGHT_BRACE ) )
  {
    fail( "`,` or `}`" );
    return nullptr;
  }
  list->span = spanning( open.span(), close.span() );
  return list;
}

void Parser::parseSwitchBody( Statement& control )
{
  advance();
  while ( !at( TokenKind::RIGHT_BRACE ) && !at( TokenKind::END_OF_FILE ) )
  {
    Token const first = peek();
    if ( atKeyword( Keyword::CASE ) || atKeyword( Keyword::DEFAULT ) )
    {
      advance();
      ExpressionPtr label;
      if ( first.keyword == Keyword::CASE )
      {
        label = parseBinary( LOWEST_PRECEDENCE );
        if ( label == nullptr )
        {
          recover( false );
          continue;
        }
      }
      Token const colon = peek();
      if ( !match( TokenKind::COLON ) )
      {
        fail( "`:`" );
        recover( false );
        continue;
      }
      control.items.push_back( Statement{ .kind = StatementKind::CASE,
                                          .span = spanning( first.span(), colon.span() ),
                                          .expression = std::move( label ),
                                          .initial = {},
                                          .step = {},
                                          .items = {},
                                          .declaration = {} } );
      continue;
    }

    // A statement before the first label is one nothing reaches, and the
    // subset has no use for it.
    if ( control.items.empty() )
    {
      fail( "`case` or `default`" );
      recover( false );
      continue;
    }
    if ( std::optional<Statement> item = parseStatement(); item.has_value() )
    {
      control.items.back().items.push_back( std::move( *item ) );
    }
  }
  Token const close = peek();
  if ( !match( TokenKind::RIGHT_BRACE ) )
  {
    fail( "`}`" );
  }
  control.span = close.span();
}

Statement Parser::parseBody()
{
  Token const first = peek();
  if ( std::optional<Statement> body = parseStatement(); body.has_value() )
  {
    return std::move( *body );
  }
  return Statement{ .kind = StatementKind::NULL_STATEMENT,
                    .span = first.span(),
                    .expression = {},
                    .initial = {},
                    .step = {},
                    .items = {},
                    .declaration = {} };
}

ExpressionPtr Parser::parseConditional()
{
  ExpressionPtr condition = parseBinary( LOWEST_PRECEDENCE );
  if ( condition == nullptr || !at( TokenKind::QUESTION ) )
  {
    return condition;
  }
  Token const op = advance();

  // The ways are conditionals of their own, so `a ? b : c ? d : e` reads as
  // C's does, the second `?` taking what is left — see
  // docs/decisions/0169-a-conditional-operator.md.
  ExpressionPtr whenTrue = parseConditional();
  if ( whenTrue == nullptr )
  {
    return nullptr;
  }
  if ( !match( TokenKind::COLON ) )
  {
    fail( "`:`" );
    return nullptr;
  }
  ExpressionPtr whenFalse = parseConditional();
  if ( whenFalse == nullptr )
  {
    return nullptr;
  }
  std::uint32_t const depth = std::max( { condition->depth, whenTrue->depth, whenFalse->depth } ) + 1;
  if ( depth > MAX_EXPRESSION_DEPTH )
  {
    refuseDepth( op, "operators", MAX_EXPRESSION_DEPTH );
    return nullptr;
  }
  ExpressionPtr node = makeExpression( ExpressionKind::CONDITIONAL, op, spanning( condition->span, whenFalse->span ) );
  node->depth = depth;
  node->left = std::move( condition );
  node->arguments.push_back( std::move( whenTrue ) );
  node->arguments.push_back( std::move( whenFalse ) );
  return node;
}

ExpressionPtr Parser::parseAssignment()
{
  ExpressionPtr target = parseConditional();
  std::optional<TokenKind> const operated = target == nullptr ? std::nullopt : compounded( peek().kind );
  if ( target == nullptr || ( !at( TokenKind::EQUAL ) && !operated.has_value() ) )
  {
    return target;
  }
  Token const op = advance();

  // `a = b = c` descends once per `=`, so the count is held before the descent
  // and not only on the tree it builds.
  if ( mAssignments == MAX_EXPRESSION_DEPTH )
  {
    refuseDepth( op, "operators", MAX_EXPRESSION_DEPTH );
    return nullptr;
  }
  ++mAssignments;
  ExpressionPtr value = parseAssignment();
  --mAssignments;
  if ( value == nullptr )
  {
    return nullptr;
  }

  std::uint32_t const depth = std::max( target->depth, value->depth ) + 1;
  if ( depth > MAX_EXPRESSION_DEPTH )
  {
    refuseDepth( op, "operators", MAX_EXPRESSION_DEPTH );
    return nullptr;
  }
  // `a += b` is `a = a OP b`, the target written twice and reached once: the
  // operator's own node carries the compound token, with the simple operator's
  // kind, so that everything holding an operator to its operands holds this
  // one — see docs/decisions/0158-a-compound-assignment-and-a-step.md.
  if ( operated.has_value() )
  {
    value = operating( *operated, op, copyOf( *target ), std::move( value ) );
    if ( value == nullptr )
    {
      return nullptr;
    }
  }
  ExpressionPtr node = makeExpression( ExpressionKind::ASSIGNMENT, op, spanning( target->span, value->span ) );
  node->depth = depth + ( operated.has_value() ? 1 : 0 );
  node->compound = operated.has_value();
  node->left = std::move( target );
  node->right = std::move( value );
  return node;
}

ExpressionPtr Parser::operating( TokenKind kind, Token const& written, ExpressionPtr left, ExpressionPtr right )
{
  std::uint32_t const depth = std::max( left->depth, right->depth ) + 1;
  if ( depth > MAX_EXPRESSION_DEPTH )
  {
    refuseDepth( written, "operators", MAX_EXPRESSION_DEPTH );
    return nullptr;
  }
  Token operated = written;
  operated.kind = kind;
  ExpressionPtr node = makeExpression( ExpressionKind::BINARY, operated, spanning( left->span, right->span ) );
  node->depth = depth;
  node->left = std::move( left );
  node->right = std::move( right );
  return node;
}

/// `++a`, `a--` and their kin: `a = a + 1` or `a = a - 1`, which is what the
/// subset has of them — see
/// docs/decisions/0158-a-compound-assignment-and-a-step.md.
ExpressionPtr Parser::parseStep( Token const& op, ExpressionPtr target, bool writtenAfter )
{
  if ( target == nullptr )
  {
    return nullptr;
  }
  ExpressionPtr one = makeExpression( ExpressionKind::INTEGER_CONSTANT, op, op.span() );
  one->value = 1;
  ExpressionPtr value = operating(
      op.kind == TokenKind::PLUS_PLUS ? TokenKind::PLUS : TokenKind::MINUS, op, copyOf( *target ), std::move( one ) );
  if ( value == nullptr )
  {
    return nullptr;
  }
  ExpressionPtr node = makeExpression( ExpressionKind::ASSIGNMENT, op, spanning( target->span, value->span ) );
  node->depth = value->depth + 1;
  node->compound = true;
  node->writtenAfter = writtenAfter;
  node->left = std::move( target );
  node->right = std::move( value );
  return node;
}

ExpressionPtr Parser::parseBinary( int minimum )
{
  ExpressionPtr left = parseUnary();
  while ( left != nullptr )
  {
    int const precedence = precedenceOf( peek().kind );
    if ( precedence < minimum )
    {
      break;
    }
    Token const op = advance();
    ExpressionPtr right = parseBinary( precedence + 1 );
    if ( right == nullptr )
    {
      return nullptr;
    }

    std::uint32_t const depth = std::max( left->depth, right->depth ) + 1;
    if ( depth > MAX_EXPRESSION_DEPTH )
    {
      refuseDepth( op, "operators", MAX_EXPRESSION_DEPTH );
      return nullptr;
    }
    ExpressionPtr node = makeExpression( ExpressionKind::BINARY, op, spanning( left->span, right->span ) );
    node->depth = depth;
    node->left = std::move( left );
    node->right = std::move( right );
    left = std::move( node );
  }
  return left;
}

ExpressionPtr Parser::parseUnary()
{
  // Gathered in a loop rather than by descending once for each, so that what
  // is held to the limit is the tree they build.
  struct Prefix
  {
    ExpressionKind kind = ExpressionKind::UNARY;
    Token op;
    diag::SourceSpan start;
    bool pointer = false;
    bool pointeeConst = false;
    bool pointeeVolatile = false;
  };

  if ( at( TokenKind::PLUS_PLUS ) || at( TokenKind::MINUS_MINUS ) )
  {
    Token const op = advance();
    return parseStep( op, parseUnary(), false );
  }

  std::vector<Prefix> operators;
  for ( ;; )
  {
    if ( at( TokenKind::MINUS ) || at( TokenKind::TILDE ) || at( TokenKind::BANG ) || at( TokenKind::STAR ) ||
         at( TokenKind::AMPERSAND ) )
    {
      Token const op = advance();
      ExpressionKind kind = ExpressionKind::UNARY;
      if ( op.kind == TokenKind::STAR )
      {
        kind = ExpressionKind::DEREFERENCE;
      }
      else if ( op.kind == TokenKind::AMPERSAND )
      {
        kind = ExpressionKind::ADDRESS;
      }
      operators.push_back( Prefix{ .kind = kind,
                                   .op = op,
                                   .start = op.span(),
                                   .pointer = false,
                                   .pointeeConst = false,
                                   .pointeeVolatile = false } );
      continue;
    }
    // A cast names its type by a keyword, which is what tells it from a
    // parenthesised expression without a table of names — see
    // docs/decisions/0084-widths-data-arrays-pointers-aggregates.md.
    std::optional<TypeName> const type = parenthesisedType();
    if ( !type.has_value() )
    {
      break;
    }
    Token const open = peek();
    mIndex += type->length;
    operators.push_back( Prefix{ .kind = ExpressionKind::CAST,
                                 .op = type->keyword,
                                 .start = open.span(),
                                 .pointer = type->pointer,
                                 .pointeeConst = type->pointeeConst,
                                 .pointeeVolatile = type->pointeeVolatile } );
  }
  ExpressionPtr node = atKeyword( Keyword::SIZEOF ) ? parseSizeof() : parsePostfix();
  for ( auto op = operators.rbegin(); node != nullptr && op != operators.rend(); ++op )
  {
    if ( node->depth == MAX_EXPRESSION_DEPTH )
    {
      refuseDepth( op->op, "operators", MAX_EXPRESSION_DEPTH );
      return nullptr;
    }
    ExpressionPtr unary = makeExpression( op->kind, op->op, spanning( op->start, node->span ) );
    unary->depth = node->depth + 1;
    unary->pointer = op->pointer;
    unary->pointeeConst = op->pointeeConst;
    unary->pointeeVolatile = op->pointeeVolatile;
    unary->left = std::move( node );
    node = std::move( unary );
  }
  return node;
}

std::optional<Parser::TypeName> Parser::parenthesisedType()
{
  if ( !at( TokenKind::LEFT_PAREN ) )
  {
    return std::nullopt;
  }
  std::size_t ahead = 1;
  bool pointeeConst = false;
  bool pointeeVolatile = false;
  auto const qualifiers = [&]
  {
    for ( ; isQualifier( peekAhead( ahead ) ); ++ahead )
    {
      bool& written = peekAhead( ahead ).keyword == Keyword::CONST_QUALIFIER ? pointeeConst : pointeeVolatile;
      if ( written )
      {
        return;
      }
      written = true;
    }
  };
  qualifiers();
  Token const keyword = peekAhead( ahead++ );
  bool const spelledOtherwise = keyword.keyword == Keyword::INT || keyword.keyword == Keyword::CHAR;
  if ( keyword.kind != TokenKind::KEYWORD || ( !isType( keyword.keyword ) && !spelledOtherwise ) )
  {
    return std::nullopt;
  }
  qualifiers();
  bool const pointer = peekAhead( ahead ).kind == TokenKind::STAR;
  ahead += pointer ? 1 : 0;
  // A qualifier written about a pointee there is none of is no type name.
  if ( peekAhead( ahead ).kind != TokenKind::RIGHT_PAREN || ( ( pointeeConst || pointeeVolatile ) && !pointer ) )
  {
    return std::nullopt;
  }
  if ( spelledOtherwise )
  {
    mSink->add( diagnostic( diag::DiagnosticId::C_TYPE_SPELLED_OTHERWISE )
                    .at( keyword.location, keyword.length )
                    .arg( "found", std::string{ mSources->textOf( keyword.span() ) } )
                    .arg( "spelling", std::string{ keyword.keyword == Keyword::INT ? "i16" : "u8" } ) );
  }
  return TypeName{ .keyword = keyword,
                   .pointer = pointer,
                   .pointeeConst = pointeeConst,
                   .pointeeVolatile = pointeeVolatile,
                   .length = ahead + 1 };
}

ExpressionPtr Parser::parseSizeof()
{
  Token const keyword = advance();
  if ( std::optional<TypeName> const type = parenthesisedType(); type.has_value() )
  {
    Token const close = peekAhead( type->length - 1 );
    mIndex += type->length;
    ExpressionPtr node =
        makeExpression( ExpressionKind::SIZEOF, type->keyword, spanning( keyword.span(), close.span() ) );
    node->pointer = type->pointer;
    node->pointeeConst = type->pointeeConst;
    node->pointeeVolatile = type->pointeeVolatile;
    return node;
  }
  ExpressionPtr operand = parseUnary();
  if ( operand == nullptr )
  {
    return nullptr;
  }
  if ( operand->depth == MAX_EXPRESSION_DEPTH )
  {
    refuseDepth( keyword, "operators", MAX_EXPRESSION_DEPTH );
    return nullptr;
  }
  ExpressionPtr node = makeExpression( ExpressionKind::SIZEOF, keyword, spanning( keyword.span(), operand->span ) );
  node->depth = operand->depth + 1;
  node->left = std::move( operand );
  return node;
}

ExpressionPtr Parser::parsePostfix()
{
  ExpressionPtr node = parsePrimary();
  if ( node == nullptr )
  {
    return nullptr;
  }

  // `f()()` and `t[i]` are built in this loop without descending, so the
  // tree it builds is what is held to the limit.
  while ( at( TokenKind::LEFT_PAREN ) || at( TokenKind::LEFT_BRACKET ) || at( TokenKind::DOT ) ||
          at( TokenKind::ARROW ) )
  {
    if ( at( TokenKind::DOT ) || at( TokenKind::ARROW ) )
    {
      Token const op = advance();
      Token const member = peek();
      if ( !match( TokenKind::IDENTIFIER ) )
      {
        fail( "a member" );
        return nullptr;
      }
      if ( node->depth == MAX_EXPRESSION_DEPTH )
      {
        refuseDepth( op, "operators", MAX_EXPRESSION_DEPTH );
        return nullptr;
      }
      ExpressionPtr access = makeExpression( ExpressionKind::MEMBER, op, spanning( node->span, member.span() ) );
      access->depth = node->depth + 1;
      access->left = std::move( node );
      access->right = makeExpression( ExpressionKind::IDENTIFIER, member, member.span() );
      node = std::move( access );
      continue;
    }
    if ( at( TokenKind::LEFT_BRACKET ) )
    {
      // An index is a parenthesis it is read inside, as an argument list is.
      Token const open = advance();
      if ( mParentheses == MAX_EXPRESSION_DEPTH )
      {
        refuseDepth( open, "parentheses", MAX_EXPRESSION_DEPTH );
        return nullptr;
      }
      ++mParentheses;
      ExpressionPtr index = parseAssignment();
      --mParentheses;
      if ( index == nullptr )
      {
        return nullptr;
      }
      Token const close = peek();
      if ( !match( TokenKind::RIGHT_BRACKET ) )
      {
        fail( "`]`" );
        return nullptr;
      }
      std::uint32_t const depth = std::max( node->depth, index->depth );
      if ( depth == MAX_EXPRESSION_DEPTH )
      {
        refuseDepth( open, "operators", MAX_EXPRESSION_DEPTH );
        return nullptr;
      }
      ExpressionPtr element = makeExpression( ExpressionKind::INDEX, open, spanning( node->span, close.span() ) );
      element->depth = depth + 1;
      element->left = std::move( node );
      element->right = std::move( index );
      node = std::move( element );
      continue;
    }
    Token const open = advance();

    // An argument list is a parenthesis the arguments are read inside, and
    // counts as one against the limit, since reading them is a descent.
    std::vector<ExpressionPtr> arguments;
    std::uint32_t depth = node->depth;
    if ( !at( TokenKind::RIGHT_PAREN ) )
    {
      if ( mParentheses == MAX_EXPRESSION_DEPTH )
      {
        refuseDepth( open, "parentheses", MAX_EXPRESSION_DEPTH );
        return nullptr;
      }
      ++mParentheses;
      for ( ;; )
      {
        ExpressionPtr argument = parseAssignment();
        if ( argument == nullptr )
        {
          --mParentheses;
          return nullptr;
        }
        depth = std::max( depth, argument->depth );
        arguments.push_back( std::move( argument ) );
        if ( !match( TokenKind::COMMA ) )
        {
          break;
        }
      }
      --mParentheses;
    }
    Token const close = peek();
    if ( !match( TokenKind::RIGHT_PAREN ) )
    {
      fail( arguments.empty() ? "`)`" : "`,` or `)`" );
      return nullptr;
    }
    if ( depth == MAX_EXPRESSION_DEPTH )
    {
      refuseDepth( open, "operators", MAX_EXPRESSION_DEPTH );
      return nullptr;
    }
    ExpressionPtr call = makeExpression( ExpressionKind::CALL, open, spanning( node->span, close.span() ) );
    call->depth = depth + 1;
    call->left = std::move( node );
    call->arguments = std::move( arguments );
    node = std::move( call );
  }
  // `a++` and `a--` are the assignment they stand for, and take nothing after
  // them: `a++ ++` is refused where the grammar wants an operator.
  if ( node != nullptr && ( at( TokenKind::PLUS_PLUS ) || at( TokenKind::MINUS_MINUS ) ) )
  {
    Token const op = advance();
    return parseStep( op, std::move( node ), true );
  }

  return node;
}

ExpressionPtr Parser::parsePrimary()
{
  Token const token = peek();
  switch ( token.kind )
  {
  case TokenKind::IDENTIFIER:
  {
    advance();
    if ( !at( TokenKind::COLON_COLON ) )
    {
      return makeExpression( ExpressionKind::IDENTIFIER, token, token.span() );
    }
    advance();
    Token const member = peek();
    if ( !match( TokenKind::IDENTIFIER ) )
    {
      fail( "an enumerator" );
      return nullptr;
    }
    ExpressionPtr node =
        makeExpression( ExpressionKind::QUALIFIED_NAME, member, spanning( token.span(), member.span() ) );
    node->left = makeExpression( ExpressionKind::IDENTIFIER, token, token.span() );
    node->right = makeExpression( ExpressionKind::IDENTIFIER, member, member.span() );
    return node;
  }
  case TokenKind::STRING_LITERAL:
    return parseStringLiteral();
  case TokenKind::CHARACTER_CONSTANT:
    return parseCharacterConstant();
  case TokenKind::INTEGER_CONSTANT:
  {
    advance();
    ExpressionPtr node = makeExpression( ExpressionKind::INTEGER_CONSTANT, token, token.span() );
    // The lexer let only a representable constant through as one.
    node->value = valueOfIntegerConstant( mSources->textOf( token.span() ) ).value_or( 0 );
    return node;
  }
  case TokenKind::LEFT_PAREN:
  {
    if ( mParentheses == MAX_EXPRESSION_DEPTH )
    {
      refuseDepth( token, "parentheses", MAX_EXPRESSION_DEPTH );
      return nullptr;
    }
    advance();
    ++mParentheses;
    ExpressionPtr inner = parseAssignment();
    --mParentheses;
    if ( inner == nullptr )
    {
      return nullptr;
    }
    Token const close = peek();
    if ( !match( TokenKind::RIGHT_PAREN ) )
    {
      fail( "`)`" );
      return nullptr;
    }
    inner->parenthesised = true;
    inner->span = spanning( token.span(), close.span() );
    return inner;
  }
  case TokenKind::KEYWORD:
    if ( token.keyword == Keyword::NULLPTR )
    {
      advance();
      return makeExpression( ExpressionKind::NULL_POINTER, token, token.span() );
    }
    if ( token.keyword == Keyword::TRUE_CONSTANT || token.keyword == Keyword::FALSE_CONSTANT )
    {
      advance();
      ExpressionPtr node = makeExpression( ExpressionKind::PREDEFINED_CONSTANT, token, token.span() );
      node->value = token.keyword == Keyword::TRUE_CONSTANT ? 1 : 0;
      return node;
    }
    fail( "an expression" );
    return nullptr;
  default:
    fail( "an expression" );
    return nullptr;
  }
}

TranslationUnit parse( diag::SourceManager const& sources, diag::FileId file, diag::DiagnosticSink& sink )
{
  std::vector<Token> const tokens = tokenize( sources, file, sink );
  Parser parser{ sources, tokens, sink };
  return parser.parseTranslationUnit();
}

} // namespace nga::c
