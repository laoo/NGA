#pragma once

#include "nga/model/Build.hpp"
#include "nga/model/MemoryMap.hpp"
#include "nga/model/Symbols.hpp"

#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace nga::model
{

/// A run of the Container's bytes that one thing accounts for.
///
/// A run is as long as what explains it: a Chunk's bytes are one run and a
/// segment's address pair is another. Nothing is left over — what belongs to
/// no Section is named for what it is, since those bytes are exactly the ones
/// a reader cannot find in any source file.
struct ByteRun
{
  std::uint32_t at = 0;      ///< Where in the Container's bytes it begins.
  std::uint32_t length = 0;  ///< How many bytes.
  std::uint32_t address = 0; ///< Where they land in memory; meaningless for a header.

  /// What the run is. `section` names a Section's bytes and carries the rest
  /// of these fields; the others carry none of them, and are what the
  /// Container itself put there.
  std::string what;

  std::string section;
  std::string module;
  std::int32_t chunk = -1; ///< Which statement of that Section, or -1.

  /// The line the bytes were written from, empty where nothing wrote them.
  SymbolSite source;
};

/// One segment of a DOS binary load file: a start address, an end address and
/// the bytes between them -- see docs/spec/xex.md.
struct ContainerSegment
{
  std::uint32_t at = 0; ///< Where the segment's header begins in the file.
  std::uint32_t start = 0;
  std::uint32_t end = 0; ///< Inclusive, as the format has it.
  std::vector<ByteRun> runs;
};

/// A Container read back as what it holds.
struct ContainerFacts
{
  std::string container;
  std::uint32_t size = 0;
  std::vector<ByteRun> header; ///< `$FF $FF`, and anything else outside a segment.
  std::vector<ContainerSegment> segments;

  /// What the runs do not account for, which must be nothing.
  std::uint32_t unaccounted = 0;
};

/// Reads a `.xex` back: its segments, and what wrote every byte of them.
///
/// The structure is read from the bytes rather than composed a second time
/// from the model, so that the facts describe the file that exists and not a
/// belief about it -- the Container writes more than Sections, and a second
/// writer here would be a second thing to keep in step. What each byte *means*
/// comes from the model: the Layout says which Section stands at an address,
/// and the Sizes say which of its statements wrote which byte.
///
/// A Container that is not a `.xex` gives back its size and nothing else, for
/// now: the diskette waits, and says so by leaving `segments` empty.
ContainerFacts containerFactsOf( Patched const& build,
                                 MemoryMap const& map,
                                 std::span<std::uint8_t const> bytes,
                                 std::string_view container );

} // namespace nga::model
