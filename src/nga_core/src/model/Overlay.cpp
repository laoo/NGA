#include "nga/model/Overlay.hpp"

#include <utility>

namespace nga::model
{

OverlaidFiles::OverlaidFiles( FileReader& under, Canonical canonical )
    : mUnder( &under ), mCanonical( std::move( canonical ) )
{
}

void OverlaidFiles::overlay( std::filesystem::path const& path, std::string text )
{
  mTexts.insert_or_assign( mCanonical( path ).generic_string(), std::move( text ) );
}

std::optional<std::string> OverlaidFiles::read( std::filesystem::path const& path )
{
  // A build with nothing overlaid asks nothing of the disk it would not have
  // asked anyway.
  if ( !mTexts.empty() )
  {
    auto const found = mTexts.find( mCanonical( path ).generic_string() );
    if ( found != mTexts.end() )
    {
      return found->second;
    }
  }
  return mUnder->read( path );
}

} // namespace nga::model
