#include "nga/diag/SourceManager.hpp"

#include <algorithm>
#include <cassert>
#include <iterator>

namespace nga::diag
{

FileId SourceManager::addFile( std::string path, std::string contents )
{
  File file;
  file.path = std::move( path );
  file.contents = std::move( contents );
  file.start = mNextStart;
  file.rank = static_cast<std::uint32_t>( mFiles.size() );

  file.lineStarts.push_back( 0 );
  for ( std::size_t i = 0; i < file.contents.size(); ++i )
  {
    if ( file.contents[i] == '\n' )
    {
      file.lineStarts.push_back( static_cast<std::uint32_t>( i + 1 ) );
    }
  }

  // One extra offset per file lets a location point just past the last byte
  // without colliding with the next file.
  mNextStart += static_cast<std::uint32_t>( file.contents.size() ) + 1;

  mFiles.push_back( std::move( file ) );
  return FileId{ static_cast<std::uint32_t>( mFiles.size() - 1 ) };
}

FileId SourceManager::addFileAfter( FileId after, std::string path, std::string contents )
{
  std::uint32_t const rank = mFiles.at( after.value ).rank;
  std::uint32_t follower = 0;
  for ( File const& file : mFiles )
  {
    if ( file.rank == rank )
    {
      follower = std::max( follower, file.follower );
    }
  }

  FileId const added = addFile( std::move( path ), std::move( contents ) );
  mFiles[added.value].rank = rank;
  mFiles[added.value].follower = follower + 1;
  mFiles[added.value].anchor = after.value;
  return added;
}

std::optional<FileId> SourceManager::registeredAfter( SourceLocation location ) const
{
  std::size_t const index = indexOfFileContaining( location );
  if ( index == mFiles.size() )
  {
    return std::nullopt;
  }
  std::optional<std::uint32_t> const anchor = mFiles[index].anchor;
  if ( !anchor.has_value() )
  {
    return std::nullopt;
  }
  return FileId{ *anchor };
}

void SourceManager::addSourceMark( SourceLocation from, std::string path, std::uint32_t line )
{
  std::size_t const index = indexOfFileContaining( from );
  assert( index < mFiles.size() && "a source mark stands in a registered file" );
  File& file = mFiles[index];
  Mark mark{ .offset = from.rawOffset() - file.start, .path = std::move( path ), .line = line };
  // Marks arrive in file order from one Module; kept sorted regardless, so
  // the lookup below may rely on it.
  auto const at = std::ranges::upper_bound( file.marks, mark.offset, {}, &Mark::offset );
  file.marks.insert( at, std::move( mark ) );
}

std::optional<SourceReference> SourceManager::sourceOf( SourceLocation location ) const
{
  std::size_t const index = indexOfFileContaining( location );
  if ( index == mFiles.size() )
  {
    return std::nullopt;
  }
  File const& file = mFiles[index];
  std::uint32_t const offsetInFile = location.rawOffset() - file.start;
  auto const after = std::ranges::upper_bound( file.marks, offsetInFile, {}, &Mark::offset );
  if ( after == file.marks.begin() )
  {
    return std::nullopt;
  }
  Mark const& mark = *std::prev( after );
  return SourceReference{ .path = mark.path, .line = mark.line };
}

std::optional<FileId> SourceManager::fileContaining( SourceLocation location ) const
{
  std::size_t const index = indexOfFileContaining( location );
  if ( index == mFiles.size() )
  {
    return std::nullopt;
  }
  return FileId{ static_cast<std::uint32_t>( index ) };
}

std::string_view SourceManager::pathOf( FileId file ) const
{
  return mFiles.at( file.value ).path;
}

std::string_view SourceManager::contentsOf( FileId file ) const
{
  return mFiles.at( file.value ).contents;
}

SourceLocation SourceManager::locationOf( FileId file, std::uint32_t offsetInFile ) const
{
  File const& entry = mFiles.at( file.value );
  assert( offsetInFile <= entry.contents.size() && "offset past the end of the file" );
  return SourceLocation::fromRawOffset( entry.start + offsetInFile );
}

std::size_t SourceManager::indexOfFileContaining( SourceLocation location ) const
{
  if ( !location.isValid() )
  {
    return mFiles.size();
  }

  // Files are appended with increasing start offsets, so the file we want is
  // the last one starting at or before the location.
  auto const after = std::ranges::upper_bound( mFiles, location.rawOffset(), {}, &File::start );
  if ( after == mFiles.begin() )
  {
    return mFiles.size();
  }

  auto const index = static_cast<std::size_t>( std::distance( mFiles.begin(), std::prev( after ) ) );
  if ( location.rawOffset() > mFiles[index].start + mFiles[index].contents.size() )
  {
    return mFiles.size();
  }
  return index;
}

std::uint64_t SourceManager::orderKeyFor( SourceLocation location ) const
{
  std::size_t const index = indexOfFileContaining( location );
  if ( index == mFiles.size() )
  {
    return 0;
  }

  // Sixteen million files and 255 followers of one are more than a Project
  // registers; the offset keeps the whole low half, as a file may be that long.
  static constexpr std::uint64_t RANK_SHIFT = 40;
  static constexpr std::uint64_t FOLLOWER_SHIFT = 32;
  File const& file = mFiles[index];
  assert( file.rank < ( 1U << 24U ) && file.follower < ( 1U << 8U ) && "the ordering key has room for this file" );
  return ( static_cast<std::uint64_t>( file.rank ) << RANK_SHIFT ) |
         ( static_cast<std::uint64_t>( file.follower ) << FOLLOWER_SHIFT ) | ( location.rawOffset() - file.start );
}

ExpandedLocation SourceManager::expand( SourceLocation location ) const
{
  std::size_t const index = indexOfFileContaining( location );
  if ( index == mFiles.size() )
  {
    return {};
  }

  File const* file = &mFiles[index];
  std::uint32_t const offsetInFile = location.rawOffset() - file->start;
  auto const after = std::ranges::upper_bound( file->lineStarts, offsetInFile );
  auto const lineIndex = static_cast<std::uint32_t>( std::distance( file->lineStarts.begin(), after ) - 1 );

  return ExpandedLocation{
    .path = file->path,
    .line = lineIndex + 1,
    .column = offsetInFile - file->lineStarts[lineIndex] + 1,
  };
}

std::string_view SourceManager::textOf( SourceSpan span ) const
{
  std::size_t const index = indexOfFileContaining( span.begin );
  if ( index == mFiles.size() )
  {
    return {};
  }

  File const& file = mFiles[index];
  std::uint32_t const offsetInFile = span.begin.rawOffset() - file.start;
  std::uint32_t const length =
      std::min( span.length, static_cast<std::uint32_t>( file.contents.size() ) - offsetInFile );
  return std::string_view{ file.contents }.substr( offsetInFile, length );
}

std::string_view SourceManager::lineTextAt( SourceLocation location ) const
{
  std::size_t const index = indexOfFileContaining( location );
  if ( index == mFiles.size() )
  {
    return {};
  }

  File const* file = &mFiles[index];
  std::uint32_t const offsetInFile = location.rawOffset() - file->start;
  auto const after = std::ranges::upper_bound( file->lineStarts, offsetInFile );
  auto const lineIndex = static_cast<std::size_t>( std::distance( file->lineStarts.begin(), after ) - 1 );

  std::uint32_t const begin = file->lineStarts[lineIndex];
  std::uint32_t const end = lineIndex + 1 < file->lineStarts.size()
                                ? file->lineStarts[lineIndex + 1] - 1
                                : static_cast<std::uint32_t>( file->contents.size() );

  std::string_view line{ file->contents };
  line = line.substr( begin, end - begin );

  // A file with CRLF endings would otherwise drag the carriage return into
  // every rendered excerpt.
  if ( line.ends_with( '\r' ) )
  {
    line.remove_suffix( 1 );
  }
  return line;
}

} // namespace nga::diag
