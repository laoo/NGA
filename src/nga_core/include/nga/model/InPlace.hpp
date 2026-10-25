#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <span>

namespace nga::model
{

/// What has been written against what has been read, for a decoder followed
/// here a byte at a time: before byte `k` of the form is read, `written - k` is
/// how far the output has got past where that byte would stand were the form
/// loaded at the destination, and the worst of it is what the margin pays for.
/// The decoding itself is the format's; this is only the count.
class InPlace
{
public:
  explicit InPlace( std::span<std::uint8_t const> form ) : mForm( form ) {}

  [[nodiscard]] bool more() const
  {
    return mAt < mForm.size();
  }

  /// The next byte of the form, zero past its end.
  std::uint8_t read()
  {
    mWorst = std::max( mWorst, static_cast<std::int64_t>( mWritten ) - static_cast<std::int64_t>( mAt ) );
    return mAt < mForm.size() ? mForm[mAt++] : 0;
  }

  void wrote( std::uint64_t count )
  {
    mWritten += count;
  }

  [[nodiscard]] std::uint64_t written() const
  {
    return mWritten;
  }

  /// The bytes the form must stand past the end of the output to be decoded
  /// where it lands; nothing where the decoder did not read it to its end.
  [[nodiscard]] std::optional<std::uint32_t> margin() const
  {
    if ( mAt != mForm.size() )
    {
      return std::nullopt;
    }
    std::int64_t const needed =
        mWorst + static_cast<std::int64_t>( mForm.size() ) - static_cast<std::int64_t>( mWritten );
    return static_cast<std::uint32_t>( std::max<std::int64_t>( 0, needed ) );
  }

private:
  std::span<std::uint8_t const> mForm;
  std::size_t mAt = 0;
  std::uint64_t mWritten = 0;
  std::int64_t mWorst = std::numeric_limits<std::int64_t>::min();
};

} // namespace nga::model
