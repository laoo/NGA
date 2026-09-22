#include "nga/c/Token.hpp"

#include <algorithm>
#include <array>
#include <span>

namespace nga::c
{

namespace
{

struct KeywordSpelling
{
  std::string_view spelling;
  Keyword keyword;
};

/// Sorted by spelling, which the assertion below holds it to, so that a lookup
/// is a binary search.
constexpr std::array KEYWORDS = {
  KeywordSpelling{ .spelling = "_Alignas", .keyword = Keyword::ALIGNAS },
  KeywordSpelling{ .spelling = "_Alignof", .keyword = Keyword::ALIGNOF },
  KeywordSpelling{ .spelling = "_Atomic", .keyword = Keyword::ATOMIC },
  KeywordSpelling{ .spelling = "_BitInt", .keyword = Keyword::BIT_INT },
  KeywordSpelling{ .spelling = "_Bool", .keyword = Keyword::BOOL },
  KeywordSpelling{ .spelling = "_Complex", .keyword = Keyword::COMPLEX },
  KeywordSpelling{ .spelling = "_Decimal128", .keyword = Keyword::DECIMAL128 },
  KeywordSpelling{ .spelling = "_Decimal32", .keyword = Keyword::DECIMAL32 },
  KeywordSpelling{ .spelling = "_Decimal64", .keyword = Keyword::DECIMAL64 },
  KeywordSpelling{ .spelling = "_Generic", .keyword = Keyword::GENERIC },
  KeywordSpelling{ .spelling = "_Imaginary", .keyword = Keyword::IMAGINARY },
  KeywordSpelling{ .spelling = "_Noreturn", .keyword = Keyword::NORETURN },
  KeywordSpelling{ .spelling = "_Static_assert", .keyword = Keyword::STATIC_ASSERT },
  KeywordSpelling{ .spelling = "_Thread_local", .keyword = Keyword::THREAD_LOCAL },
  KeywordSpelling{ .spelling = "alignas", .keyword = Keyword::ALIGNAS },
  KeywordSpelling{ .spelling = "alignof", .keyword = Keyword::ALIGNOF },
  KeywordSpelling{ .spelling = "auto", .keyword = Keyword::AUTO },
  KeywordSpelling{ .spelling = "bool", .keyword = Keyword::BOOL },
  KeywordSpelling{ .spelling = "break", .keyword = Keyword::BREAK },
  KeywordSpelling{ .spelling = "case", .keyword = Keyword::CASE },
  KeywordSpelling{ .spelling = "char", .keyword = Keyword::CHAR },
  KeywordSpelling{ .spelling = "const", .keyword = Keyword::CONST_QUALIFIER },
  KeywordSpelling{ .spelling = "constexpr", .keyword = Keyword::CONSTEXPR },
  KeywordSpelling{ .spelling = "continue", .keyword = Keyword::CONTINUE },
  KeywordSpelling{ .spelling = "default", .keyword = Keyword::DEFAULT },
  KeywordSpelling{ .spelling = "do", .keyword = Keyword::DO },
  KeywordSpelling{ .spelling = "double", .keyword = Keyword::DOUBLE },
  KeywordSpelling{ .spelling = "else", .keyword = Keyword::ELSE },
  KeywordSpelling{ .spelling = "enum", .keyword = Keyword::ENUM },
  KeywordSpelling{ .spelling = "extern", .keyword = Keyword::EXTERN },
  KeywordSpelling{ .spelling = "false", .keyword = Keyword::FALSE_CONSTANT },
  KeywordSpelling{ .spelling = "float", .keyword = Keyword::FLOAT },
  KeywordSpelling{ .spelling = "for", .keyword = Keyword::FOR },
  KeywordSpelling{ .spelling = "goto", .keyword = Keyword::GOTO },
  KeywordSpelling{ .spelling = "i16", .keyword = Keyword::I16 },
  KeywordSpelling{ .spelling = "i8", .keyword = Keyword::I8 },
  KeywordSpelling{ .spelling = "if", .keyword = Keyword::IF },
  KeywordSpelling{ .spelling = "inline", .keyword = Keyword::INLINE },
  KeywordSpelling{ .spelling = "int", .keyword = Keyword::INT },
  KeywordSpelling{ .spelling = "long", .keyword = Keyword::LONG },
  KeywordSpelling{ .spelling = "nullptr", .keyword = Keyword::NULLPTR },
  KeywordSpelling{ .spelling = "register", .keyword = Keyword::REGISTER },
  KeywordSpelling{ .spelling = "restrict", .keyword = Keyword::RESTRICT },
  KeywordSpelling{ .spelling = "return", .keyword = Keyword::RETURN },
  KeywordSpelling{ .spelling = "short", .keyword = Keyword::SHORT },
  KeywordSpelling{ .spelling = "signed", .keyword = Keyword::SIGNED },
  KeywordSpelling{ .spelling = "sizeof", .keyword = Keyword::SIZEOF },
  KeywordSpelling{ .spelling = "static", .keyword = Keyword::STATIC },
  KeywordSpelling{ .spelling = "static_assert", .keyword = Keyword::STATIC_ASSERT },
  KeywordSpelling{ .spelling = "struct", .keyword = Keyword::STRUCT },
  KeywordSpelling{ .spelling = "switch", .keyword = Keyword::SWITCH },
  KeywordSpelling{ .spelling = "thread_local", .keyword = Keyword::THREAD_LOCAL },
  KeywordSpelling{ .spelling = "true", .keyword = Keyword::TRUE_CONSTANT },
  KeywordSpelling{ .spelling = "typedef", .keyword = Keyword::TYPEDEF },
  KeywordSpelling{ .spelling = "typeof", .keyword = Keyword::TYPEOF },
  KeywordSpelling{ .spelling = "typeof_unqual", .keyword = Keyword::TYPEOF_UNQUAL },
  KeywordSpelling{ .spelling = "u16", .keyword = Keyword::U16 },
  KeywordSpelling{ .spelling = "u8", .keyword = Keyword::U8 },
  KeywordSpelling{ .spelling = "union", .keyword = Keyword::UNION },
  KeywordSpelling{ .spelling = "unsigned", .keyword = Keyword::UNSIGNED },
  KeywordSpelling{ .spelling = "void", .keyword = Keyword::VOID_TYPE },
  KeywordSpelling{ .spelling = "volatile", .keyword = Keyword::VOLATILE },
  KeywordSpelling{ .spelling = "while", .keyword = Keyword::WHILE },
};

static_assert( std::ranges::is_sorted( KEYWORDS, {}, &KeywordSpelling::spelling ), "KEYWORDS must stay sorted" );

} // namespace

Keyword keywordOf( std::string_view spelling )
{
  // Searched through a span, whose iterator is a class on every standard
  // library, rather than the array, whose iterator is a pointer on some and not
  // on others.
  std::span<KeywordSpelling const> const table{ KEYWORDS };
  auto const found = std::ranges::lower_bound( table, spelling, {}, &KeywordSpelling::spelling );
  return found != table.end() && found->spelling == spelling ? found->keyword : Keyword::NONE;
}

std::string_view nameOf( TokenKind kind )
{
  switch ( kind )
  {
  case TokenKind::END_OF_FILE:
    return "end of file";
  case TokenKind::COMMENT:
    return "comment";
  case TokenKind::UNKNOWN:
    return "unknown";
  case TokenKind::IDENTIFIER:
    return "identifier";
  case TokenKind::KEYWORD:
    return "keyword";
  case TokenKind::INTEGER_CONSTANT:
    return "integer constant";
  case TokenKind::STRING_LITERAL:
    return "string literal";
  case TokenKind::CHARACTER_CONSTANT:
    return "character constant";
  case TokenKind::LEFT_BRACKET:
    return "[";
  case TokenKind::RIGHT_BRACKET:
    return "]";
  case TokenKind::LEFT_PAREN:
    return "(";
  case TokenKind::RIGHT_PAREN:
    return ")";
  case TokenKind::LEFT_BRACE:
    return "{";
  case TokenKind::RIGHT_BRACE:
    return "}";
  case TokenKind::DOT:
    return ".";
  case TokenKind::ARROW:
    return "->";
  case TokenKind::PLUS_PLUS:
    return "++";
  case TokenKind::MINUS_MINUS:
    return "--";
  case TokenKind::AMPERSAND:
    return "&";
  case TokenKind::STAR:
    return "*";
  case TokenKind::PLUS:
    return "+";
  case TokenKind::MINUS:
    return "-";
  case TokenKind::TILDE:
    return "~";
  case TokenKind::BANG:
    return "!";
  case TokenKind::SLASH:
    return "/";
  case TokenKind::PERCENT:
    return "%";
  case TokenKind::LESS_LESS:
    return "<<";
  case TokenKind::GREATER_GREATER:
    return ">>";
  case TokenKind::LESS:
    return "<";
  case TokenKind::GREATER:
    return ">";
  case TokenKind::LESS_EQUAL:
    return "<=";
  case TokenKind::GREATER_EQUAL:
    return ">=";
  case TokenKind::EQUAL_EQUAL:
    return "==";
  case TokenKind::BANG_EQUAL:
    return "!=";
  case TokenKind::CARET:
    return "^";
  case TokenKind::PIPE:
    return "|";
  case TokenKind::AMPERSAND_AMPERSAND:
    return "&&";
  case TokenKind::PIPE_PIPE:
    return "||";
  case TokenKind::QUESTION:
    return "?";
  case TokenKind::COLON:
    return ":";
  case TokenKind::COLON_COLON:
    return "::";
  case TokenKind::SEMICOLON:
    return ";";
  case TokenKind::ELLIPSIS:
    return "...";
  case TokenKind::EQUAL:
    return "=";
  case TokenKind::STAR_EQUAL:
    return "*=";
  case TokenKind::SLASH_EQUAL:
    return "/=";
  case TokenKind::PERCENT_EQUAL:
    return "%=";
  case TokenKind::PLUS_EQUAL:
    return "+=";
  case TokenKind::MINUS_EQUAL:
    return "-=";
  case TokenKind::LESS_LESS_EQUAL:
    return "<<=";
  case TokenKind::GREATER_GREATER_EQUAL:
    return ">>=";
  case TokenKind::AMPERSAND_EQUAL:
    return "&=";
  case TokenKind::CARET_EQUAL:
    return "^=";
  case TokenKind::PIPE_EQUAL:
    return "|=";
  case TokenKind::COMMA:
    return ",";
  case TokenKind::HASH:
    return "#";
  case TokenKind::HASH_HASH:
    return "##";
  }
  return "unknown";
}

} // namespace nga::c
