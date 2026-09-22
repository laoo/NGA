#include "nga/syntax/TokenCursor.hpp"

namespace nga::syntax
{

TokenCursor::TokenCursor( std::span<Token const> tokens, bool skipLineEnds )
    : mTokens( tokens ), mSkipLineEnds( skipLineEnds )
{
  settle();
}

bool TokenCursor::isRetained( Token const& token ) const
{
  if ( token.kind == TokenKind::COMMENT )
  {
    return false;
  }
  return !mSkipLineEnds || token.kind != TokenKind::LINE_END;
}

void TokenCursor::settle()
{
  while ( mIndex + 1 < mTokens.size() && !isRetained( mTokens[mIndex] ) )
  {
    ++mIndex;
  }
}

Token const& TokenCursor::peek( std::uint32_t ahead ) const
{
  // tokenize() always ends the stream with an end-of-file token, so the last
  // element is the right answer for every request past the end.
  static constexpr Token END_OF_STREAM{};
  if ( mTokens.empty() )
  {
    return END_OF_STREAM;
  }

  std::size_t index = mIndex;
  std::uint32_t remaining = ahead;
  while ( index + 1 < mTokens.size() )
  {
    if ( isRetained( mTokens[index] ) )
    {
      if ( remaining == 0 )
      {
        return mTokens[index];
      }
      --remaining;
    }
    ++index;
  }
  return mTokens.back();
}

bool TokenCursor::atLineEnd() const
{
  TokenKind const here = peek().kind;
  return here == TokenKind::LINE_END || here == TokenKind::END_OF_FILE;
}

Token TokenCursor::advance()
{
  Token const taken = peek();
  if ( mIndex + 1 < mTokens.size() )
  {
    ++mIndex;
    settle();
  }
  return taken;
}

bool TokenCursor::match( TokenKind wanted )
{
  if ( !at( wanted ) )
  {
    return false;
  }
  advance();
  return true;
}

void TokenCursor::skipToNextLine()
{
  while ( !atEnd() && peek().kind != TokenKind::LINE_END )
  {
    advance();
  }
  match( TokenKind::LINE_END );
}

} // namespace nga::syntax
