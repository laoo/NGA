#include "nga/syntax/Token.hpp"

namespace nga::syntax
{

std::string_view nameOf( TokenKind kind )
{
  switch ( kind )
  {
  case TokenKind::END_OF_FILE:
    return "end of file";
  case TokenKind::LINE_END:
    return "line end";
  case TokenKind::COMMENT:
    return "comment";
  case TokenKind::UNKNOWN:
    return "unknown";
  case TokenKind::IDENTIFIER:
    return "identifier";
  case TokenKind::LOCAL_IDENTIFIER:
    return "local identifier";
  case TokenKind::ANONYMOUS_LABEL:
    return "anonymous label";
  case TokenKind::NUMBER:
    return "number";
  case TokenKind::CHARACTER:
    return "character literal";
  case TokenKind::STRING:
    return "string literal";
  case TokenKind::HASH:
    return "#";
  case TokenKind::LEFT_PAREN:
    return "(";
  case TokenKind::RIGHT_PAREN:
    return ")";
  case TokenKind::LEFT_BRACKET:
    return "[";
  case TokenKind::RIGHT_BRACKET:
    return "]";
  case TokenKind::LEFT_BRACE:
    return "{";
  case TokenKind::RIGHT_BRACE:
    return "}";
  case TokenKind::COMMA:
    return ",";
  case TokenKind::COLON:
    return ":";
  case TokenKind::QUESTION:
    return "?";
  case TokenKind::DOT:
    return ".";
  case TokenKind::DOT_DOT:
    return "..";
  case TokenKind::ELLIPSIS:
    return "...";
  case TokenKind::EQUAL:
    return "=";
  case TokenKind::EQUAL_EQUAL:
    return "==";
  case TokenKind::BANG:
    return "!";
  case TokenKind::BANG_EQUAL:
    return "!=";
  case TokenKind::LESS:
    return "<";
  case TokenKind::LESS_EQUAL:
    return "<=";
  case TokenKind::LESS_LESS:
    return "<<";
  case TokenKind::GREATER:
    return ">";
  case TokenKind::GREATER_EQUAL:
    return ">=";
  case TokenKind::GREATER_GREATER:
    return ">>";
  case TokenKind::AMPERSAND:
    return "&";
  case TokenKind::AMPERSAND_AMPERSAND:
    return "&&";
  case TokenKind::PIPE:
    return "|";
  case TokenKind::PIPE_PIPE:
    return "||";
  case TokenKind::PLUS:
    return "+";
  case TokenKind::MINUS:
    return "-";
  case TokenKind::STAR:
    return "*";
  case TokenKind::SLASH:
    return "/";
  case TokenKind::CARET:
    return "^";
  case TokenKind::TILDE:
    return "~";
  }
  return "unknown";
}

} // namespace nga::syntax
