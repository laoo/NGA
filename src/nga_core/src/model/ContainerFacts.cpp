#include "nga/model/ContainerFacts.hpp"

#include "nga/model/Car.hpp"

#include "nga/model/MemoryMap.hpp"
#include "nga/model/Module.hpp"
#include "nga/model/Segments.hpp"

#include <algorithm>
#include <cstddef>
#include <optional>
#include <utility>
#include <vector>

namespace nga::model
{

namespace
{

std::uint32_t wordAt( std::span<std::uint8_t const> bytes, std::size_t at )
{
  return static_cast<std::uint32_t>( bytes[at] ) | ( static_cast<std::uint32_t>( bytes[at + 1] ) << 8U );
}

/// The Section standing at an address in the Phase the program starts in,
/// which is the Phase a `.xex` loads.
/// The Section standing at an address, among those a Phase holds. Nothing for
/// `entry` asks for **any** Phase's, which is what ROM wants: nothing loads it,
/// so a Section standing there stands there for the whole run whatever Phase
/// needs it.
MapEntry const* sectionAt( MemoryMap const& map, std::optional<PhaseIndex> entry, std::uint32_t address )
{
  for ( MapEntry const& held : map.entries )
  {
    if ( address < held.begin || address >= held.end )
    {
      continue;
    }
    // A Section in no Phase stands somewhere all the same, and a Section of
    // the entry Phase is what a loader puts in memory.
    bool const live = !entry.has_value() || !held.firstPhase.has_value() || !held.lastPhase.has_value() ||
                      ( entry->value >= held.firstPhase.value().value && entry->value <= held.lastPhase.value().value );
    if ( live )
    {
      return &held;
    }
  }
  return nullptr;
}

/// Which statement of a Section wrote the byte at `offset` into it, and where
/// that statement was written.
struct Wrote
{
  std::int32_t chunk = -1;
  std::uint32_t begin = 0;
  std::uint32_t end = 0;
  diag::SourceSpan span;
};

Wrote chunkAt( Patched const& build, SectionRef where, std::uint32_t offset )
{
  Module const& module = build.modules()[where.module.value];
  Section const& section = module.sectionAt( where.section );
  Sizes const& sizes = build.sizes();

  for ( std::size_t i = 0; i < section.chunks().size(); ++i )
  {
    ChunkIndex const index{ static_cast<std::uint32_t>( i ) };
    std::uint32_t const begin = sizes.offsetOf( where, index );
    std::uint32_t const size = sizes.sizeOfChunk( where, index );
    if ( size != 0 && offset >= begin && offset < begin + size )
    {
      return {
        .chunk = static_cast<std::int32_t>( i ), .begin = begin, .end = begin + size, .span = section.chunks()[i].span
      };
    }
  }
  return {};
}

std::optional<SymbolSite> siteOf( diag::SourceManager const& sources, diag::SourceSpan span )
{
  if ( !span.begin.isValid() )
  {
    return std::nullopt;
  }
  std::optional<diag::FileId> const file = sources.fileContaining( span.begin );
  if ( !file.has_value() )
  {
    return std::nullopt;
  }
  std::uint32_t const base = sources.locationOf( *file, 0 ).rawOffset();
  return SymbolSite{ .file = *file, .offset = span.begin.rawOffset() - base, .length = span.length };
}

/// What the Container itself wrote at an address, whatever Section the Layout
/// has standing there.
///
/// `RUNAD` and `INITAD` are inside the OS Module's own Section on an Atari,
/// and attributing them to it would be true of the addresses and false about
/// the bytes: the Container put the program's start there, and no statement of
/// `os.asm` did.
std::string_view containerWrote( std::uint32_t address )
{
  if ( address == RUNAD || address == RUNAD + 1 )
  {
    return "runad";
  }
  if ( address == INITAD || address == INITAD + 1 )
  {
    return "initad";
  }
  return {};
}

/// What a byte outside every Section is, named rather than left blank: these
/// are exactly the bytes a reader cannot find in any source file, and the ones
/// they will ask about first.
std::string_view outsideName( std::uint32_t address )
{
  std::string_view const wrote = containerWrote( address );
  return wrote.empty() ? std::string_view{ "storage image" } : wrote;
}

} // namespace

/// A run of bytes that one Section's statement wrote, or the whole Section
/// where no statement did.
ByteRun runOf( Patched const& build,
               MapEntry const& held,
               diag::SourceManager const& sources,
               std::uint32_t at,
               std::uint32_t address,
               std::uint32_t limit )
{
  Wrote const wrote = chunkAt( build, held.where, address - held.begin );
  std::uint32_t const end = wrote.chunk < 0 ? std::min( held.end, limit ) : std::min( held.begin + wrote.end, limit );

  ByteRun run{ .at = at,
               .length = end - address,
               .address = address,
               .what = "section",
               .section = held.name,
               .module = held.module,
               .chunk = wrote.chunk,
               .source = {} };
  if ( std::optional<SymbolSite> const site = siteOf( sources, wrote.span ); site.has_value() )
  {
    run.source = *site;
  }
  return run;
}

/// One stretch of bytes at known addresses, attributed Section by Section and
/// statement by statement: a segment of a load file, or the boot record a
/// diskette begins with.
void attribute( Patched const& build,
                MemoryMap const& map,
                std::uint32_t at,
                std::uint32_t address,
                std::uint32_t length,
                std::vector<ByteRun>& out,
                std::uint32_t& accounted,
                std::optional<PhaseIndex> live = std::nullopt )
{
  diag::SourceManager const& sources = build.sources();
  std::optional<PhaseIndex> const entry = live.has_value() ? live : std::optional{ build.project().phases.entry };
  std::uint32_t const last = address + length;

  while ( address < last )
  {
    std::uint32_t const offsetInFile = at + ( address - ( last - length ) );
    MapEntry const* const held = containerWrote( address ).empty() ? sectionAt( map, entry, address ) : nullptr;

    if ( held == nullptr )
    {
      std::uint32_t runEnd = address;
      std::string_view const name = outsideName( address );
      while ( runEnd < last && outsideName( runEnd ) == name &&
              ( !containerWrote( runEnd ).empty() || sectionAt( map, entry, runEnd ) == nullptr ) )
      {
        ++runEnd;
      }
      out.push_back( ByteRun{ .at = offsetInFile,
                              .length = runEnd - address,
                              .address = address,
                              .what = std::string{ name },
                              .section = {},
                              .module = {},
                              .chunk = -1,
                              .source = {} } );
      accounted += runEnd - address;
      address = runEnd;
      continue;
    }

    ByteRun const run = runOf( build, *held, sources, offsetInFile, address, last );
    accounted += run.length;
    address += run.length;
    out.push_back( run );
  }
}

// Where a sector begins in the file. The first three hold 128 bytes and the
// rest hold 256: the OS boots with `DSCTLN` at 128, so a diskette's boot
// record is 128-byte sectors whatever the rest of it is -- see
// docs/spec/atr.md.
constexpr std::uint32_t ATR_HEADER = 16;
constexpr std::uint32_t BOOT_SECTORS = 3;
constexpr std::uint32_t BOOT_SECTOR_SIZE = 128;
constexpr std::uint32_t SECTOR_SIZE = 256;
constexpr std::uint32_t BOOT_ADDRESS = 0x0700;
constexpr std::uint32_t STORAGE_FIRST_SECTOR = 4;

std::uint32_t offsetOfSector( std::uint32_t sector )
{
  if ( sector <= BOOT_SECTORS )
  {
    return ATR_HEADER + ( ( sector - 1 ) * BOOT_SECTOR_SIZE );
  }
  return ATR_HEADER + ( BOOT_SECTORS * BOOT_SECTOR_SIZE ) + ( ( sector - BOOT_SECTORS - 1 ) * SECTOR_SIZE );
}

/// Reads the stream of segments a DOS binary load file is. The `.xex` is this
/// from its second byte; a diskette's load image is the same stream, from
/// wherever the boot record says it begins.
std::uint32_t readSegments( Patched const& build,
                            MemoryMap const& map,
                            std::span<std::uint8_t const> bytes,
                            std::size_t at,
                            ContainerFacts& facts )
{
  std::uint32_t accounted = 0;
  while ( at + 4 <= bytes.size() )
  {
    ContainerSegment segment;
    segment.at = static_cast<std::uint32_t>( at );
    segment.start = wordAt( bytes, at );
    segment.end = wordAt( bytes, at + 2 );

    // `$FFFF` closes a diskette's image, since no segment begins there.
    if ( segment.start == 0xFFFF )
    {
      facts.header.push_back( ByteRun{ .at = segment.at,
                                       .length = 2,
                                       .address = 0,
                                       .what = "end of image",
                                       .section = {},
                                       .module = {},
                                       .chunk = -1,
                                       .source = {} } );
      return accounted + 2;
    }

    at += 4;
    accounted += 4;

    std::uint32_t const length = segment.end >= segment.start ? segment.end - segment.start + 1 : 0;
    if ( at + length > bytes.size() )
    {
      break;
    }

    attribute( build, map, static_cast<std::uint32_t>( at ), segment.start, length, segment.runs, accounted );
    at += length;
    facts.segments.push_back( std::move( segment ) );
  }
  return accounted;
}

/// A cartridge, read back: the sixteen-byte header, the part of the image the
/// CPU always sees, and the banks.
///
/// Nothing is loaded, so there is no stream of segments to walk and no address
/// to take a run's meaning from outside the fixed part: a bank holds stored
/// forms, which have no address until something copies them. What no Section
/// and no Payload accounts for is the `$FF` an erased device holds, named
/// where it stands.
ContainerFacts cartridge( Patched const& build,
                          MemoryMap const& map,
                          std::span<std::uint8_t const> bytes,
                          ContainerFacts facts,
                          Cartridge const& board )
{
  constexpr std::uint32_t header = 16;
  std::uint32_t accounted = header;
  facts.header.push_back( ByteRun{ .at = 0,
                                   .length = header,
                                   .address = 0,
                                   .what = "container header",
                                   .section = {},
                                   .module = {},
                                   .chunk = -1,
                                   .source = {} } );

  // The fixed part, at the addresses the CPU reads it at. Every Phase's
  // Sections, not the entry Phase's alone: ROM has no Residency.
  ContainerSegment fixed;
  fixed.at = header + board.fixedOffset;
  fixed.start = board.fixed.begin;
  fixed.end = board.fixed.end - 1;
  attribute( build, map, fixed.at, board.fixed.begin, board.fixed.size(), fixed.runs, accounted, std::nullopt );
  facts.segments.push_back( std::move( fixed ) );

  // The banks: every Payload, every Frame and every Pane's Section with bytes,
  // at the position storage gave it, which on this board is the offset in the
  // image. No addresses, because a stored form has none.
  std::vector<ByteRun> stored;
  Target const& target = build.target();
  std::optional<std::uint32_t> const firstState =
      target.storageUnits.has_value() && build.project().driver.has_value() &&
              build.project().driver->stream.has_value()
          ? target.windows[build.project().driver->stream->value].firstStateOf( *target.storageUnits, target.unitSets )
          : std::optional<std::uint32_t>{};
  for ( MapEntry const& held : map.entries )
  {
    if ( held.waits.has_value() && held.storedSize > 0 )
    {
      stored.push_back( ByteRun{ .at = header + ( held.waits->bank.value * map.bankSize ) + held.waits->offset,
                                 .length = held.storedSize,
                                 .address = 0,
                                 .what = held.transform.empty() ? "payload" : "payload, " + held.transform,
                                 .section = held.name,
                                 .module = held.module,
                                 .chunk = -1,
                                 .source = {} } );
      continue;
    }
    // A Pane's Section stands in its Bank at its own offset within the Window,
    // which is where the Container wrote it and where it is read from.
    if ( !held.pane.empty() && firstState.has_value() && held.paneState >= *firstState )
    {
      std::uint32_t const bank = held.paneState - *firstState;
      std::uint32_t const window = target.windows[target.panes[0].window.value].ranges.front().begin;
      stored.push_back( ByteRun{ .at = header + ( bank * map.bankSize ) + ( held.begin - window ),
                                 .length = held.end - held.begin,
                                 .address = held.begin,
                                 .what = "pane",
                                 .section = held.name,
                                 .module = held.module,
                                 .chunk = -1,
                                 .source = {} } );
    }
  }
  for ( MapFrame const& frame : map.frames )
  {
    stored.push_back( ByteRun{ .at = header + ( frame.waits.bank.value * map.bankSize ) + frame.waits.offset,
                               .length = frame.size,
                               .address = 0,
                               .what = "frame",
                               .section = frame.name,
                               .module = {},
                               .chunk = -1,
                               .source = {} } );
  }
  std::ranges::sort( stored, []( ByteRun const& a, ByteRun const& b ) { return a.at < b.at; } );
  if ( !stored.empty() )
  {
    ContainerSegment banks;
    banks.at = stored.front().at;
    banks.start = 0;
    banks.end = 0;
    for ( ByteRun const& run : stored )
    {
      accounted += run.length;
    }
    banks.runs = std::move( stored );
    facts.segments.push_back( std::move( banks ) );
  }

  // What is left is what an erased device holds, named where it stands: a
  // cartridge is exactly as large as its board, so the emptiness is in the
  // middle of it as often as at the end.
  std::vector<std::pair<std::uint32_t, std::uint32_t>> taken;
  std::size_t runs = facts.header.size();
  for ( ContainerSegment const& segment : facts.segments )
  {
    runs += segment.runs.size();
  }
  taken.reserve( runs );
  for ( ByteRun const& run : facts.header )
  {
    taken.emplace_back( run.at, run.length );
  }
  for ( ContainerSegment const& segment : facts.segments )
  {
    for ( ByteRun const& run : segment.runs )
    {
      taken.emplace_back( run.at, run.length );
    }
  }
  std::ranges::sort( taken );

  std::vector<ByteRun> erased;
  std::uint32_t reached = 0;
  for ( auto const& [at, length] : taken )
  {
    if ( at > reached )
    {
      erased.push_back( ByteRun{ .at = reached,
                                 .length = at - reached,
                                 .address = 0,
                                 .what = "erased",
                                 .section = {},
                                 .module = {},
                                 .chunk = -1,
                                 .source = {} } );
      accounted += at - reached;
    }
    reached = std::max( reached, at + length );
  }
  if ( reached < bytes.size() )
  {
    erased.push_back( ByteRun{ .at = reached,
                               .length = static_cast<std::uint32_t>( bytes.size() ) - reached,
                               .address = 0,
                               .what = "erased",
                               .section = {},
                               .module = {},
                               .chunk = -1,
                               .source = {} } );
    accounted += static_cast<std::uint32_t>( bytes.size() ) - reached;
  }
  facts.header.insert( facts.header.end(), erased.begin(), erased.end() );
  std::ranges::sort( facts.header, []( ByteRun const& a, ByteRun const& b ) { return a.at < b.at; } );

  // In the order they stand in the file: the banks come first on every board,
  // since the fixed part is the last of them.
  std::ranges::sort( facts.segments,
                     []( ContainerSegment const& a, ContainerSegment const& b ) { return a.at < b.at; } );

  facts.unaccounted = facts.size - std::min( accounted, facts.size );
  return facts;
}

/// A diskette, read back: the boot record the tool wrote, the sectors storage
/// is, and the load image after them.
///
/// The image's own sector is the one thing the Container patches into the
/// record, so it is read from there rather than worked out again: the
/// alternative is to sum what PlaceStorage used and hope the two agree.
ContainerFacts
diskette( Patched const& build, MemoryMap const& map, std::span<std::uint8_t const> bytes, ContainerFacts facts )
{
  std::uint32_t accounted = ATR_HEADER;
  facts.header.push_back( ByteRun{ .at = 0,
                                   .length = ATR_HEADER,
                                   .address = 0,
                                   .what = "container header",
                                   .section = {},
                                   .module = {},
                                   .chunk = -1,
                                   .source = {} } );

  // The record is the Sections of `nga.boot` at the addresses their pins gave
  // them, read into `$0700` as three sectors of 128 bytes.
  ContainerSegment boot;
  boot.at = ATR_HEADER;
  boot.start = BOOT_ADDRESS;
  boot.end = BOOT_ADDRESS + ( BOOT_SECTORS * BOOT_SECTOR_SIZE ) - 1;
  attribute( build, map, ATR_HEADER, BOOT_ADDRESS, BOOT_SECTORS * BOOT_SECTOR_SIZE, boot.runs, accounted );
  facts.segments.push_back( std::move( boot ) );

  // Where the image begins, taken from the record where the Container patched
  // it: `ngaBootImage` holds the sector as two bytes.
  std::uint32_t imageSector = 0;
  for ( MapEntry const& held : map.entries )
  {
    if ( held.name == "ngaBootImage" && held.begin >= BOOT_ADDRESS )
    {
      std::size_t const where = ATR_HEADER + ( held.begin - BOOT_ADDRESS );
      if ( where + 1 < bytes.size() )
      {
        imageSector = wordAt( bytes, where );
      }
      break;
    }
  }

  std::uint32_t const storageAt = offsetOfSector( STORAGE_FIRST_SECTOR );
  std::uint32_t const imageAt =
      imageSector >= STORAGE_FIRST_SECTOR ? offsetOfSector( imageSector ) : static_cast<std::uint32_t>( bytes.size() );

  // Storage: every Payload and every Frame at the position PlaceStorage gave
  // it, end to end from sector four. A unit is 65536 bytes, which is what
  // makes the model's three-byte address the hardware's.
  std::vector<ByteRun> stored;
  for ( MapEntry const& held : map.entries )
  {
    if ( !held.waits.has_value() || held.storedSize == 0 )
    {
      continue;
    }
    std::uint32_t const at = storageAt + ( held.waits->bank.value * map.bankSize ) + held.waits->offset;
    stored.push_back( ByteRun{ .at = at,
                               .length = held.storedSize,
                               .address = held.begin,
                               .what = held.transform.empty() ? "payload" : "payload, " + held.transform,
                               .section = held.name,
                               .module = held.module,
                               .chunk = -1,
                               .source = {} } );
  }
  for ( MapFrame const& frame : map.frames )
  {
    stored.push_back( ByteRun{ .at = storageAt + ( frame.waits.bank.value * map.bankSize ) + frame.waits.offset,
                               .length = frame.size,
                               .address = 0,
                               .what = "frame",
                               .section = frame.name,
                               .module = {},
                               .chunk = -1,
                               .source = {} } );
  }
  std::ranges::sort( stored, []( ByteRun const& a, ByteRun const& b ) { return a.at < b.at; } );

  if ( !stored.empty() )
  {
    ContainerSegment storage;
    storage.at = storageAt;
    storage.start = 0;
    storage.end = 0;
    for ( ByteRun const& run : stored )
    {
      accounted += run.length;
    }
    storage.runs = std::move( stored );
    facts.segments.push_back( std::move( storage ) );
  }

  if ( imageAt < bytes.size() )
  {
    accounted += readSegments( build, map, bytes, imageAt, facts );
  }

  // What is left is the diskette itself: at least the 720 sectors a diskette
  // has, whatever the program needs, so that an ordinary program comes out as
  // something a drive would read. There are two such stretches — the sectors
  // of storage nothing waits in, and everything past the image — and each is
  // named where it stands rather than summed at the end, because the first
  // one is in the middle of the file.
  std::vector<std::pair<std::uint32_t, std::uint32_t>> taken;
  taken.reserve( facts.header.size() + facts.segments.size() );
  for ( ByteRun const& run : facts.header )
  {
    taken.emplace_back( run.at, run.length );
  }
  for ( ContainerSegment const& segment : facts.segments )
  {
    if ( segment.at >= imageAt )
    {
      taken.emplace_back( segment.at, 4 );
    }
    for ( ByteRun const& run : segment.runs )
    {
      taken.emplace_back( run.at, run.length );
    }
  }
  std::ranges::sort( taken );

  std::vector<ByteRun> gaps;
  std::uint32_t reached = 0;
  for ( auto const& [at, length] : taken )
  {
    if ( at > reached )
    {
      gaps.push_back( ByteRun{ .at = reached,
                               .length = at - reached,
                               .address = 0,
                               .what = "unused sectors",
                               .section = {},
                               .module = {},
                               .chunk = -1,
                               .source = {} } );
    }
    reached = std::max( reached, at + length );
  }
  if ( reached < facts.size )
  {
    gaps.push_back( ByteRun{ .at = reached,
                             .length = facts.size - reached,
                             .address = 0,
                             .what = "unused sectors",
                             .section = {},
                             .module = {},
                             .chunk = -1,
                             .source = {} } );
  }
  for ( ByteRun const& gap : gaps )
  {
    accounted += gap.length;
  }
  facts.header.insert( facts.header.end(), gaps.begin(), gaps.end() );

  facts.unaccounted = facts.size - std::min( accounted, facts.size );
  return facts;
}

ContainerFacts containerFactsOf( Patched const& build,
                                 MemoryMap const& map,
                                 std::span<std::uint8_t const> bytes,
                                 std::string_view container )
{
  ContainerFacts facts;
  facts.container = std::string{ container };
  facts.size = static_cast<std::uint32_t>( bytes.size() );

  if ( bytes.size() < 2 )
  {
    facts.unaccounted = facts.size;
    return facts;
  }

  if ( container == "xex" )
  {
    facts.header.push_back( ByteRun{ .at = 0,
                                     .length = 2,
                                     .address = 0,
                                     .what = "container header",
                                     .section = {},
                                     .module = {},
                                     .chunk = -1,
                                     .source = {} } );
    facts.unaccounted = facts.size - std::min( 2 + readSegments( build, map, bytes, 2, facts ), facts.size );
    return facts;
  }

  if ( container == "atr" )
  {
    return diskette( build, map, bytes, std::move( facts ) );
  }

  if ( container == "car" )
  {
    std::optional<Cartridge> const board =
        build.project().cartridge.has_value() ? cartridgeNamed( *build.project().cartridge ) : std::nullopt;
    if ( board.has_value() )
    {
      return cartridge( build, map, bytes, std::move( facts ), *board );
    }
  }

  facts.unaccounted = facts.size;
  return facts;
}

} // namespace nga::model
