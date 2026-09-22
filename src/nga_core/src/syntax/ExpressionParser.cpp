#include "nga/syntax/ExpressionParser.hpp"

#include <optional>
#include <string>
#include <utility>

namespace nga::syntax
{

namespace
{

struct InfixOperator
{
  BinaryOperator op{};
  int power = 0;
};

/// The infix reading of a token. `<` and `>` appear here as comparisons and in
/// the prefix position as byte extraction; which one applies is decided by
/// position alone, exactly as [0006] promises.
std::optional<InfixOperator> infixOperatorFor( TokenKind kind )
{
  switch ( kind )
  {
  case TokenKind::PIPE_PIPE:
    return InfixOperator{ .op = BinaryOperator::LOGICAL_OR, .power = 10 };
  case TokenKind::AMPERSAND_AMPERSAND:
    return InfixOperator{ .op = BinaryOperator::LOGICAL_AND, .power = 20 };
  case TokenKind::PIPE:
    return InfixOperator{ .op = BinaryOperator::BITWISE_OR, .power = 30 };
  case TokenKind::CARET:
    return InfixOperator{ .op = BinaryOperator::BITWISE_XOR, .power = 40 };
  case TokenKind::AMPERSAND:
    return InfixOperator{ .op = BinaryOperator::BITWISE_AND, .power = 50 };
  case TokenKind::EQUAL_EQUAL:
    return InfixOperator{ .op = BinaryOperator::EQUAL, .power = 60 };
  case TokenKind::BANG_EQUAL:
    return InfixOperator{ .op = BinaryOperator::NOT_EQUAL, .power = 60 };
  case TokenKind::LESS:
    return InfixOperator{ .op = BinaryOperator::LESS, .power = 70 };
  case TokenKind::LESS_EQUAL:
    return InfixOperator{ .op = BinaryOperator::LESS_EQUAL, .power = 70 };
  case TokenKind::GREATER:
    return InfixOperator{ .op = BinaryOperator::GREATER, .power = 70 };
  case TokenKind::GREATER_EQUAL:
    return InfixOperator{ .op = BinaryOperator::GREATER_EQUAL, .power = 70 };
  case TokenKind::LESS_LESS:
    return InfixOperator{ .op = BinaryOperator::SHIFT_LEFT, .power = 90 };
  case TokenKind::GREATER_GREATER:
    return InfixOperator{ .op = BinaryOperator::SHIFT_RIGHT, .power = 90 };
  case TokenKind::PLUS:
    return InfixOperator{ .op = BinaryOperator::ADD, .power = 100 };
  case TokenKind::MINUS:
    return InfixOperator{ .op = BinaryOperator::SUBTRACT, .power = 100 };
  case TokenKind::STAR:
    return InfixOperator{ .op = BinaryOperator::MULTIPLY, .power = 110 };
  case TokenKind::SLASH:
    return InfixOperator{ .op = BinaryOperator::DIVIDE, .power = 110 };
  default:
    return std::nullopt;
  }
}

std::optional<UnaryOperator> prefixOperatorFor( TokenKind kind )
{
  switch ( kind )
  {
  case TokenKind::MINUS:
    return UnaryOperator::NEGATE;
  case TokenKind::TILDE:
    return UnaryOperator::COMPLEMENT;
  case TokenKind::BANG:
    return UnaryOperator::NOT;
  case TokenKind::LESS:
    return UnaryOperator::LOW_BYTE;
  case TokenKind::GREATER:
    return UnaryOperator::HIGH_BYTE;
  default:
    return std::nullopt;
  }
}

bool isByteExtraction( UnaryOperator op )
{
  return op == UnaryOperator::LOW_BYTE || op == UnaryOperator::HIGH_BYTE;
}

/// A node standing in for input that has already been reported. Everything
/// built on top of it stays quiet.
ExpressionPtr errorAt( diag::SourceSpan span )
{
  return makeExpression( ExpressionKind::ERROR, Token{}, span );
}

/// Whether a token can stand inside an expression, which is how far a skip
/// past one refused as too deep goes. A comma, a line ending and whatever
/// else a grammar ends an expression with are not among them.
bool standsInExpression( TokenKind kind )
{
  switch ( kind )
  {
  case TokenKind::IDENTIFIER:
  case TokenKind::LOCAL_IDENTIFIER:
  case TokenKind::ANONYMOUS_LABEL:
  case TokenKind::NUMBER:
  case TokenKind::CHARACTER:
  case TokenKind::STRING:
  case TokenKind::UNKNOWN:
  case TokenKind::LEFT_PAREN:
  case TokenKind::RIGHT_PAREN:
  case TokenKind::QUESTION:
  case TokenKind::COLON:
  case TokenKind::DOT:
  case TokenKind::LESS_LESS:
    return true;
  default:
    return infixOperatorFor( kind ).has_value() || prefixOperatorFor( kind ).has_value();
  }
}

} // namespace

ExpressionParser::ExpressionParser( TokenCursor& cursor, diag::DiagnosticSink& sink )
    : mCursor( &cursor ), mSink( &sink )
{
}

void ExpressionParser::report( diag::Diagnostic value ) const
{
  mSink->add( std::move( value ) );
}

ExpressionPtr ExpressionParser::parse()
{
  mOperators = 0;
  mParentheses = 0;
  mTooDeep = false;
  return parseAt( BindingPower::NONE );
}

ExpressionPtr ExpressionParser::parseAt( int minimumPower )
{
  ExpressionPtr left = parsePrefix();

  while ( true )
  {
    if ( mCursor->at( TokenKind::DOT ) && BindingPower::ATTRIBUTE >= minimumPower )
    {
      Token const dot = mCursor->advance();
      if ( !mCursor->at( TokenKind::IDENTIFIER ) )
      {
        report(
            diag::diagnostic( diag::DiagnosticId::EXPECTED_NAME ).at( dot.location, dot.length ).arg( "after", "." ) );
        return errorAt( spanning( left->span, dot.span() ) );
      }

      Token const name = mCursor->advance();
      ExpressionPtr attribute = makeExpression( ExpressionKind::ATTRIBUTE, name, spanning( left->span, name.span() ) );
      attribute->left = std::move( left );
      left = bounded( std::move( attribute ) );
      continue;
    }

    if ( mCursor->at( TokenKind::QUESTION ) && BindingPower::SELECT >= minimumPower )
    {
      left = bounded( parseSelect( std::move( left ) ) );
      continue;
    }

    std::optional<InfixOperator> const infix = infixOperatorFor( mCursor->kind() );
    if ( !infix.has_value() || infix->power < minimumPower )
    {
      return left;
    }

    Token const op = mCursor->advance();
    // Every binary operator here is left-associative, which is what the plus
    // one says: an operator of equal power binds to the left, not the right.
    ExpressionPtr right = descend( infix->power + 1, false );

    ExpressionPtr node = makeExpression( ExpressionKind::BINARY, op, spanning( left->span, right->span ) );
    node->binaryOperator = infix->op;
    node->left = std::move( left );
    node->right = std::move( right );
    if ( rejectAmbiguousMix( *node ) )
    {
      // Refused rather than resolved, so there is no reading to hand on.
      node->kind = ExpressionKind::ERROR;
    }
    left = bounded( std::move( node ) );
  }
}

ExpressionPtr ExpressionParser::parseSelect( ExpressionPtr condition )
{
  Token const question = mCursor->advance();

  // The answer between the two marks is a whole expression: `:` is no
  // operator of its own, so nothing but the mark can end it.
  ExpressionPtr taken = descend( BindingPower::NONE, false );
  if ( !mCursor->at( TokenKind::COLON ) )
  {
    if ( !mTooDeep )
    {
      Token const here = mCursor->current();
      report( diag::diagnostic( diag::DiagnosticId::EXPECTED_SELECT_COLON ).at( here.location, here.length ) );
    }
    return errorAt( spanning( condition->span, taken->span ) );
  }
  Token const colon = mCursor->advance();

  // At its own power rather than one above it, which is what makes the
  // chain right-associative: another `?` on this side is taken here.
  ExpressionPtr otherwise = descend( BindingPower::SELECT, false );

  ExpressionPtr arm = makeExpression( ExpressionKind::BINARY, colon, spanning( taken->span, otherwise->span ) );
  arm->binaryOperator = BinaryOperator::ARM;
  arm->left = std::move( taken );
  arm->right = std::move( otherwise );

  ExpressionPtr node = makeExpression( ExpressionKind::BINARY, question, spanning( condition->span, arm->span ) );
  node->binaryOperator = BinaryOperator::SELECT;
  node->left = std::move( condition );
  node->right = std::move( arm );
  return node;
}

ExpressionPtr ExpressionParser::parsePrefix()
{
  // `<<foo` is longest match applied honestly: a shift where a prefix belongs.
  // It has no legitimate meaning, so it is reported and then read as `<`, which
  // keeps one mistyped character from derailing the rest of the line.
  if ( mCursor->at( TokenKind::LESS_LESS ) )
  {
    Token const op = mCursor->advance();
    report( diag::diagnostic( diag::DiagnosticId::SHIFT_IN_PREFIX_POSITION ).at( op.location, op.length ) );
    ExpressionPtr operand = descend( BindingPower::BYTE_EXTRACTION, false );
    ExpressionPtr node = makeExpression( ExpressionKind::UNARY, op, spanning( op.span(), operand->span ) );
    node->unaryOperator = UnaryOperator::LOW_BYTE;
    node->left = std::move( operand );
    return bounded( std::move( node ) );
  }

  std::optional<UnaryOperator> const prefix = prefixOperatorFor( mCursor->kind() );
  if ( !prefix.has_value() )
  {
    return parsePrimary();
  }

  Token const op = mCursor->advance();
  int const power = isByteExtraction( *prefix ) ? BindingPower::BYTE_EXTRACTION : BindingPower::PREFIX;
  ExpressionPtr operand = descend( power, false );

  ExpressionPtr node = makeExpression( ExpressionKind::UNARY, op, spanning( op.span(), operand->span ) );
  node->unaryOperator = *prefix;
  node->left = std::move( operand );
  return bounded( std::move( node ) );
}

ExpressionPtr ExpressionParser::parsePrimary()
{
  switch ( mCursor->kind() )
  {
  case TokenKind::NUMBER:
  {
    Token const token = mCursor->advance();
    return makeExpression( ExpressionKind::NUMBER, token, token.span() );
  }
  case TokenKind::CHARACTER:
  {
    Token const token = mCursor->advance();
    return makeExpression( ExpressionKind::CHARACTER, token, token.span() );
  }
  case TokenKind::STRING:
  {
    Token const token = mCursor->advance();
    return makeExpression( ExpressionKind::STRING, token, token.span() );
  }
  case TokenKind::IDENTIFIER:
  {
    Token const token = mCursor->advance();
    return makeExpression( ExpressionKind::NAME, token, token.span() );
  }
  case TokenKind::LOCAL_IDENTIFIER:
  case TokenKind::ANONYMOUS_LABEL:
  {
    Token const token = mCursor->advance();
    return makeExpression( ExpressionKind::LOCAL_NAME, token, token.span() );
  }
  case TokenKind::DOT:
  {
    // The dot separates a scope from a name; written with nothing on its
    // left, the scope is the Module's top level. A directive is the other
    // reading of a leading dot and never meets this one: no statement begins
    // with an expression, so the two positions are disjoint.
    Token const dot = mCursor->advance();
    if ( !mCursor->at( TokenKind::IDENTIFIER ) )
    {
      report(
          diag::diagnostic( diag::DiagnosticId::EXPECTED_NAME ).at( dot.location, dot.length ).arg( "after", "." ) );
      return errorAt( dot.span() );
    }
    Token const token = mCursor->advance();
    ExpressionPtr node = makeExpression( ExpressionKind::NAME, token, spanning( dot.span(), token.span() ) );
    node->fromRoot = true;
    return node;
  }
  case TokenKind::LEFT_PAREN:
    return parseParenthesised();

  case TokenKind::UNKNOWN:
  {
    // The lexer has already said what is wrong with it. Saying anything more
    // would turn one bad character into two findings.
    Token const token = mCursor->advance();
    return errorAt( token.span() );
  }
  default:
    break;
  }

  Token const here = mCursor->current();
  report( diag::diagnostic( diag::DiagnosticId::EXPECTED_EXPRESSION ).at( here.location, here.length ) );
  // Deliberately does not consume: the statement grammar owns recovery, and
  // consuming here would swallow the token it needs to see.
  return errorAt( diag::SourceSpan{ .begin = here.location, .length = 0 } );
}

ExpressionPtr ExpressionParser::parseParenthesised()
{
  Token const open = mCursor->advance();
  ExpressionPtr inner = descend( BindingPower::NONE, true );

  if ( !mCursor->at( TokenKind::RIGHT_PAREN ) )
  {
    if ( !mTooDeep )
    {
      report( diag::diagnostic( diag::DiagnosticId::UNCLOSED_PARENTHESIS ).at( open.location, open.length ) );
    }
    inner->parenthesised = true;
    return inner;
  }

  Token const close = mCursor->advance();
  inner->parenthesised = true;
  inner->span = spanning( open.span(), close.span() );
  return inner;
}

bool ExpressionParser::rejectAmbiguousMix( Expression const& node ) const
{
  auto offending = [&node]( Expression const* child ) -> Expression const*
  {
    if ( child == nullptr || child->kind != ExpressionKind::BINARY || child->parenthesised )
    {
      return nullptr;
    }
    if ( isShift( node.binaryOperator ) && isAdditive( child->binaryOperator ) )
    {
      return child;
    }
    if ( isBitwise( node.binaryOperator ) && isComparison( child->binaryOperator ) )
    {
      return child;
    }
    return nullptr;
  };

  Expression const* child = offending( node.left.get() );
  if ( child == nullptr )
  {
    child = offending( node.right.get() );
  }
  if ( child == nullptr )
  {
    return false;
  }

  // Both of these are places where C settles the question silently and is
  // famously wrong about it half the time. Naming the two operators is the
  // whole message: refusing costs less than choosing, because it does not
  // require being right.
  report( diag::diagnostic( diag::DiagnosticId::PARENTHESES_REQUIRED )
              .at( node.span.begin, node.span.length )
              .arg( "outer", std::string{ spellingOf( node.binaryOperator ) } )
              .arg( "inner", std::string{ spellingOf( child->binaryOperator ) } ) );
  return true;
}

ExpressionPtr ExpressionParser::descend( int minimumPower, bool parenthesis )
{
  std::uint32_t& open = parenthesis ? mParentheses : mOperators;
  if ( open >= MAX_EXPRESSION_DEPTH )
  {
    // An operand under an operator ends up that operator's child, so this
    // count never passes a tree's depth and refuses nothing `bounded` would
    // let through; it only refuses before the descent rather than after.
    reportTooDeep( mCursor->current().span() );
    return errorAt( skipRest() );
  }
  ++open;
  ExpressionPtr node = parseAt( minimumPower );
  --open;
  return node;
}

ExpressionPtr ExpressionParser::bounded( ExpressionPtr node )
{
  if ( depthOf( *node ) <= MAX_EXPRESSION_DEPTH )
  {
    return node;
  }
  reportTooDeep( node->span );
  return errorAt( node->span );
}

void ExpressionParser::reportTooDeep( diag::SourceSpan at )
{
  if ( mTooDeep )
  {
    return;
  }
  mTooDeep = true;
  report( diag::diagnostic( diag::DiagnosticId::EXPRESSION_TOO_DEEP )
              .at( at.begin, at.length )
              .arg( "limit", static_cast<std::int64_t>( MAX_EXPRESSION_DEPTH ) ) );
}

diag::SourceSpan ExpressionParser::skipRest()
{
  diag::SourceSpan skipped{ .begin = mCursor->current().location, .length = 0 };
  std::uint32_t open = 0;
  while ( !mCursor->atLineEnd() && standsInExpression( mCursor->kind() ) )
  {
    TokenKind const kind = mCursor->kind();
    if ( open == 0 && ( kind == TokenKind::RIGHT_PAREN || kind == TokenKind::COLON ) )
    {
      break;
    }
    if ( kind == TokenKind::LEFT_PAREN )
    {
      ++open;
    }
    else if ( kind == TokenKind::RIGHT_PAREN )
    {
      --open;
    }
    skipped = spanning( skipped, mCursor->advance().span() );
  }
  return skipped;
}

} // namespace nga::syntax
