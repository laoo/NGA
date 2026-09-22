#pragma once

#include <compare>
#include <cstdint>

namespace nga::diag
{

/// A position in the virtual space that SourceManager lays every file out in.
///
/// This is deliberately a single 32-bit offset rather than a file/line/column
/// triple: every Chunk and every Reference carries one, so four bytes against
/// twelve decides the memory footprint of the hottest structure in the program.
/// Expansion to a human-readable position happens only when rendering.
class SourceLocation
{
public:
  SourceLocation() = default;

  static SourceLocation fromRawOffset( std::uint32_t offset )
  {
    return SourceLocation{ offset };
  }

  /// Offset zero is reserved so that a default-constructed location is invalid.
  [[nodiscard]] bool isValid() const
  {
    return mOffset != INVALID_OFFSET;
  }

  [[nodiscard]] std::uint32_t rawOffset() const
  {
    return mOffset;
  }

  friend bool operator==( SourceLocation, SourceLocation ) = default;
  friend auto operator<=>( SourceLocation, SourceLocation ) = default;

private:
  static constexpr std::uint32_t INVALID_OFFSET = 0;

  explicit SourceLocation( std::uint32_t offset ) : mOffset( offset ) {}

  std::uint32_t mOffset = INVALID_OFFSET;
};

static_assert( sizeof( SourceLocation ) == 4, "SourceLocation must stay four bytes wide" );

/// A half-open span starting at a location. A length of zero is a bare point,
/// which renders as a single caret rather than an underline.
struct SourceSpan
{
  SourceLocation begin;
  std::uint32_t length = 0;
};

} // namespace nga::diag
