#include "nga/model/Section.hpp"

#include <algorithm>
#include <string>
#include <utility>
#include <variant>

namespace nga::model
{

std::string spellingOf( DeclaredType const& type )
{
  switch ( type.kind )
  {
  case DeclaredType::Kind::I8:
    return "i8";
  case DeclaredType::Kind::U16:
    return "u16";
  case DeclaredType::Kind::I16:
    return "i16";
  case DeclaredType::Kind::BOOL:
    return "bool";
  case DeclaredType::Kind::BYTES:
    return "u8[" + std::to_string( type.count ) + "]";
  case DeclaredType::Kind::U8:
    break;
  }
  return "u8";
}

void Section::markTaking( ChunkIndex chunk, Taking taking, std::vector<syntax::Token> followers )
{
  mChunks[chunk.value].taking = taking;
  mChunks[chunk.value].followers = std::move( followers );
}

ChunkIndex Section::appendChunk( ChunkContent content, diag::SourceSpan span, std::vector<syntax::ExpressionPtr> items )
{
  Chunk chunk{ .content = std::move( content ),
               .span = span,
               .firstItem = static_cast<std::uint32_t>( mItems.size() ),
               .itemCount = static_cast<std::uint32_t>( items.size() ) };

  for ( syntax::ExpressionPtr& item : items )
  {
    mItems.push_back( std::move( item ) );
  }

  ChunkIndex const index = nextChunkIndex();
  mChunks.push_back( chunk );
  return index;
}

void Section::extendLastChunk( std::vector<syntax::ExpressionPtr> items, diag::SourceSpan span )
{
  Chunk& chunk = mChunks.back();
  chunk.itemCount += static_cast<std::uint32_t>( items.size() );
  chunk.span = syntax::spanning( chunk.span, span );
  for ( syntax::ExpressionPtr& item : items )
  {
    mItems.push_back( std::move( item ) );
  }
}

std::uint32_t Section::appendItems( std::vector<syntax::ExpressionPtr> items )
{
  auto const first = static_cast<std::uint32_t>( mItems.size() );
  for ( syntax::ExpressionPtr& item : items )
  {
    mItems.push_back( std::move( item ) );
  }
  return first;
}

std::span<Chunk const> Section::innerChunksOf( ChunkIndex index ) const
{
  auto const found = mExpansions.find( index.value );
  if ( found == mExpansions.end() )
  {
    return {};
  }
  return found->second;
}

void Section::expand( ChunkIndex use, std::vector<Chunk> inner )
{
  mExpansions.insert_or_assign( use.value, std::move( inner ) );
}

std::size_t Section::openConditional()
{
  mConditionals.emplace_back();
  return mConditionals.size() - 1;
}

void Section::blank( ChunkIndex index )
{
  Chunk& chunk = mChunks[index.value];
  chunk.content = ReserveContent{};
  chunk.itemCount = 0;

  // A use that is not taken has no inside either: Expand never gives it one,
  // and this keeps the rule for a use blanked after the fact.
  mExpansions.erase( index.value );
}

bool Section::emitsBytes() const
{
  return std::ranges::any_of(
      mChunks, []( Chunk const& chunk ) { return !std::holds_alternative<ReserveContent>( chunk.content ); } );
}

} // namespace nga::model
