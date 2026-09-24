#pragma once

#include "nga/model/Patch.hpp"

#include <cstdint>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

namespace nga::model
{

/// Where a DOS loader takes the address to jump to once a file is loaded, and
/// where it takes the address to call after the segment that wrote it. The
/// `.atr`'s boot record reads the first of them for the same reason a DOS does
/// — it is where the Container put the program's start.
constexpr std::uint32_t RUNAD = 0x02E0;
constexpr std::uint32_t INITAD = 0x02E2;

/// The sequence of **segments** a DOS binary load file is made of, which is
/// also what an `.atr`'s boot record reads from its sectors: a start address
/// and an end address, both two bytes little endian and the end inclusive,
/// followed by exactly the bytes between them.
///
/// A segment that begins where the one before it ended is not a segment of
/// its own: the bytes are appended to the one before and its end moved,
/// which is the same load four bytes shorter. A Proc is a Section, and a
/// Module of Procs would otherwise pay a header per routine.
class SegmentWriter
{
public:
  void segment( std::uint32_t start, std::span<std::uint8_t const> data );
  void byte( std::uint32_t at, std::uint8_t value );
  void word( std::uint32_t at, std::uint32_t value );

  [[nodiscard]] std::vector<std::uint8_t> take() &&
  {
    return std::move( mBytes );
  }

private:
  /// The segment being written: where it ends, and where in the stream its
  /// end address stands, so that a contiguous one can move it.
  struct Open
  {
    std::uint32_t end = 0;
    std::size_t endAt = 0;
  };

  void address( std::uint32_t value );

  std::vector<std::uint8_t> mBytes;
  std::optional<Open> mOpen;
};

/// Something with a place to wait in storage — a Payload, a Frame, or a Pane's
/// Section with bytes — and where it waits.
struct Stored
{
  StorageAddress at;
  std::span<std::uint8_t const> form;
};

/// Everything that waits in storage, by unit and then by offset: what a
/// Container writes into the units, in the order that lets it switch a unit
/// in once rather than once per image.
std::vector<Stored> storedImages( Patched const& build );

/// A Section that is in memory before anything of the program runs, and where
/// its initialised extent stands.
struct Resident
{
  SectionRef where{};
  std::uint32_t address = 0;
  std::span<std::uint8_t const> content;
};

/// The Sections the entry Phase needs that emit bytes, in Project order: what
/// a Container loads before the program starts, the Resident ones and the
/// driver among them.
///
/// `except` names Modules to leave out, which is how a Container keeps its own
/// loader out of the image that loader reads: those bytes are in the file
/// already, put there by whatever reads the Container's first sectors.
std::vector<Resident> residentSections( Patched const& build, std::span<ModuleIndex const> except );

/// Those Sections as segments, each at the address its extent starts at.
void writeResidentSegments( Patched const& build, SegmentWriter& out, std::span<ModuleIndex const> except );

/// The runtime address of a Label the whole program can see, or nothing.
std::optional<std::uint32_t>
addressOfExported( GlobalSymbols const& symbols, std::string_view name, Sizes const& sizes, Layout const& layout );

/// The bytes Patch produced for a Section's initialised extent, clamped to
/// what Patch produced: a Section shorter than its size is a defect of an
/// earlier Step and not of the writer.
std::span<std::uint8_t const> contentOf( Bytes const& bytes, SectionRef where, InitialisedExtent extent );

} // namespace nga::model
