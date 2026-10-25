#include "nga/model/Section.hpp"

#include <algorithm>
#include <array>
#include <string>
#include <utility>
#include <variant>

namespace nga::model
{

std::optional<SectionAttribute> sectionAttributeNamed( std::string_view text )
{
  static constexpr std::array<std::pair<std::string_view, SectionAttribute>, 7> NAMES{ {
      { "runtimeSectionAddress", SectionAttribute::RUNTIME_SECTION_ADDRESS },
      { "runtimeSectionSize", SectionAttribute::RUNTIME_SECTION_SIZE },
      { "resident", SectionAttribute::RESIDENT },
      { "linearSectionAddress", SectionAttribute::LINEAR_SECTION_ADDRESS },
      { "linearFamilyAddress", SectionAttribute::LINEAR_FAMILY_ADDRESS },
      { "storagePayloadAddress", SectionAttribute::STORAGE_PAYLOAD_ADDRESS },
      { "storagePayloadSize", SectionAttribute::STORAGE_PAYLOAD_SIZE },
  } };
  auto const found = std::ranges::find( NAMES, text, &std::pair<std::string_view, SectionAttribute>::first );
  return found == NAMES.end() ? std::nullopt : std::optional{ found->second };
}

void Section::markTaking( ChunkIndex chunk, syntax::Taking taking, std::vector<syntax::Token> followers )
{
  mChunks[chunk.value].taking = taking;
  mChunks[chunk.value].followers = std::move( followers );
}

void Section::markAlternatives( ChunkIndex chunk, std::vector<syntax::Token> alternatives )
{
  mChunks[chunk.value].alternatives = std::move( alternatives );
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
