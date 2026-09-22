#include "nga/syntax/Expression.hpp"

#include <string>

namespace nga::syntax
{

std::optional<std::string> dottedNameOf( diag::SourceManager const& sources, Expression const& node )
{
  if ( node.kind == ExpressionKind::NAME )
  {
    return std::string{ sources.textOf( node.token.span() ) };
  }
  if ( node.kind != ExpressionKind::ATTRIBUTE || node.left == nullptr )
  {
    return std::nullopt;
  }
  std::optional<std::string> left = dottedNameOf( sources, *node.left );
  if ( !left.has_value() )
  {
    return std::nullopt;
  }
  *left += '.';
  *left += sources.textOf( node.token.span() );
  return left;
}

Expression const& leftmostOf( Expression const& node )
{
  Expression const* at = &node;
  while ( at->kind == ExpressionKind::ATTRIBUTE && at->left != nullptr )
  {
    at = at->left.get();
  }
  return *at;
}

std::string_view nameOf( ExpressionType type )
{
  switch ( type )
  {
  case ExpressionType::UNKNOWN:
    return "unknown";
  case ExpressionType::INTEGER:
    return "integer";
  case ExpressionType::ADDRESS:
    return "address";
  case ExpressionType::STRING:
    return "string";
  case ExpressionType::PANE:
    return "pane";
  }
  return "unknown";
}

std::string_view spellingOf( UnaryOperator op )
{
  switch ( op )
  {
  case UnaryOperator::NEGATE:
    return "-";
  case UnaryOperator::COMPLEMENT:
    return "~";
  case UnaryOperator::NOT:
    return "!";
  case UnaryOperator::LOW_BYTE:
    return "<";
  case UnaryOperator::HIGH_BYTE:
    return ">";
  }
  return "?";
}

std::string_view spellingOf( BinaryOperator op )
{
  switch ( op )
  {
  case BinaryOperator::ADD:
    return "+";
  case BinaryOperator::SUBTRACT:
    return "-";
  case BinaryOperator::MULTIPLY:
    return "*";
  case BinaryOperator::DIVIDE:
    return "/";
  case BinaryOperator::SHIFT_LEFT:
    return "<<";
  case BinaryOperator::SHIFT_RIGHT:
    return ">>";
  case BinaryOperator::BITWISE_AND:
    return "&";
  case BinaryOperator::BITWISE_OR:
    return "|";
  case BinaryOperator::BITWISE_XOR:
    return "^";
  case BinaryOperator::EQUAL:
    return "==";
  case BinaryOperator::NOT_EQUAL:
    return "!=";
  case BinaryOperator::LESS:
    return "<";
  case BinaryOperator::LESS_EQUAL:
    return "<=";
  case BinaryOperator::GREATER:
    return ">";
  case BinaryOperator::GREATER_EQUAL:
    return ">=";
  case BinaryOperator::LOGICAL_AND:
    return "&&";
  case BinaryOperator::LOGICAL_OR:
    return "||";
  case BinaryOperator::SELECT:
    return "?";
  case BinaryOperator::ARM:
    return ":";
  }
  return "?";
}

bool isAdditive( BinaryOperator op )
{
  return op == BinaryOperator::ADD || op == BinaryOperator::SUBTRACT;
}

bool isShift( BinaryOperator op )
{
  return op == BinaryOperator::SHIFT_LEFT || op == BinaryOperator::SHIFT_RIGHT;
}

bool isBitwise( BinaryOperator op )
{
  return op == BinaryOperator::BITWISE_AND || op == BinaryOperator::BITWISE_OR || op == BinaryOperator::BITWISE_XOR;
}

bool isComparison( BinaryOperator op )
{
  switch ( op )
  {
  case BinaryOperator::EQUAL:
  case BinaryOperator::NOT_EQUAL:
  case BinaryOperator::LESS:
  case BinaryOperator::LESS_EQUAL:
  case BinaryOperator::GREATER:
  case BinaryOperator::GREATER_EQUAL:
    return true;
  default:
    return false;
  }
}

ExpressionPtr makeExpression( ExpressionKind kind, Token token, diag::SourceSpan span )
{
  auto node = std::make_unique<Expression>();
  node->kind = kind;
  node->token = token;
  node->span = span;
  return node;
}

bool containsError( Expression const& node )
{
  if ( node.kind == ExpressionKind::ERROR )
  {
    return true;
  }
  if ( node.left != nullptr && containsError( *node.left ) )
  {
    return true;
  }
  return node.right != nullptr && containsError( *node.right );
}

std::uint32_t depthOf( Expression const& node )
{
  std::uint32_t const left = node.left != nullptr ? depthOf( *node.left ) + 1 : 0;
  std::uint32_t const right = node.right != nullptr ? depthOf( *node.right ) + 1 : 0;
  return left > right ? left : right;
}

diag::SourceSpan spanning( diag::SourceSpan first, diag::SourceSpan second )
{
  std::uint32_t const begin = first.begin.rawOffset();
  std::uint32_t const end = second.begin.rawOffset() + second.length;
  return diag::SourceSpan{ .begin = first.begin, .length = end > begin ? end - begin : first.length };
}

} // namespace nga::syntax
