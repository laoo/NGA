#include "nga/model/ContainerFacts.hpp"

#include "nga/model/Atr.hpp"
#include "nga/model/Car.hpp"

#include "nga/model/MemoryMap.hpp"
#include "nga/model/Module.hpp"
#include "nga/model/Segments.hpp"
#include "nga/model/Transform.hpp"
#include "nga/model/Transition.hpp"

#include <algorithm>
#include <cstddef>
#include <optional>
#include <string>
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

/// Where a byte written at `address` lands in the unit set the stream's
/// Window shows, as the medium addresses it, while the load shows `shown`
/// there: nothing outside that Window, for a state of no Bank of the set, or
/// for a set the medium does not read as one memory.
struct Linear
{
  std::string_view units;
  std::uint32_t address = 0;
};

std::optional<Linear> linearAt( Patched const& build, std::uint32_t address, std::optional<std::uint32_t> shown )
{
  Project const& project = build.project();
  Target const& target = build.target();
  if ( !shown.has_value() || !project.driver.has_value() || !project.driver->stream.has_value() )
  {
    return std::nullopt;
  }
  WindowIndex const stream = *project.driver->stream;
  Window const& window = target.windows[stream.value];
  std::optional<UnitSetIndex> const set = target.unitSetShownBy( stream );
  if ( !set.has_value() || window.ranges.size() != 1 )
  {
    return std::nullopt;
  }
  UnitSet const& units = target.unitSets[set->value];
  std::optional<std::uint32_t> const first = window.firstStateOf( *set, target.unitSets );
  AddressRange const range = window.ranges.front();
  if ( !units.linear.has_value() || !first.has_value() || address < range.begin || address >= range.end ||
       *shown < *first || *shown - *first >= units.count )
  {
    return std::nullopt;
  }
  return Linear{ .units = units.name,
                 .address = units.linear->begin + ( ( *shown - *first ) * window.size() ) + ( address - range.begin ) };
}

/// A Section a byte belongs to, and where the byte is in the Section's own
/// addresses: the CPU's, or the medium's for a Section in a unit set.
struct Standing
{
  MapEntry const* held = nullptr;
  std::uint32_t at = 0;
};

/// The Section standing at an address, among those a Phase holds. Nothing for
/// `entry` asks for **any** Phase's, which is what ROM wants: nothing loads it,
/// so a Section standing there stands there for the whole run whatever Phase
/// needs it.
///
/// What a byte written into a Window is depends on the state the load showed
/// there, `shown`, the last the file named: a Pane's Section only while its
/// own state is shown, and a Section in a unit set by the linear address the
/// shown Bank gives the byte, since it has no address the CPU writes — see
/// docs/decisions/0254-a-section-may-stand-in-a-unit-set.md and
/// docs/decisions/0256-a-named-state-is-filled-where-a-xex-can-show-it.md.
Standing sectionAt( Patched const& build,
                    MemoryMap const& map,
                    std::optional<PhaseIndex> entry,
                    std::uint32_t address,
                    std::optional<std::uint32_t> shown )
{
  std::optional<Linear> const linear = linearAt( build, address, shown );
  for ( MapEntry const& held : map.entries )
  {
    std::uint32_t own = address;
    if ( !held.units.empty() )
    {
      if ( !linear.has_value() || held.units != linear->units )
      {
        continue;
      }
      own = linear->address;
    }
    if ( own < held.begin || own >= held.end )
    {
      continue;
    }
    if ( !held.pane.empty() )
    {
      // A family's Sections stand in every member, and its entry names the
      // first.
      std::optional<PaneIndex> const pane =
          build.modules()[held.where.module.value].sectionAt( held.where.section ).pane();
      std::uint32_t const count = pane.has_value() ? build.target().panes[pane->value].count : 1;
      if ( !shown.has_value() || *shown < held.paneState || *shown >= held.paneState + count )
      {
        continue;
      }
    }
    // A Section in no Phase stands somewhere all the same, and a Section of
    // the entry Phase is what a loader puts in memory.
    bool const live = !entry.has_value() || !held.firstPhase.has_value() || !held.lastPhase.has_value() ||
                      ( entry->value >= held.firstPhase.value().value && entry->value <= held.lastPhase.value().value );
    if ( live )
    {
      return Standing{ .held = &held, .at = own };
    }
  }
  return Standing{};
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
///
/// `own` is where the byte at `address` is in the Section's own addresses,
/// and `limit` where the stretch ends in the CPU's.
ByteRun runOf( Patched const& build,
               MapEntry const& held,
               diag::SourceManager const& sources,
               std::uint32_t at,
               std::uint32_t address,
               std::uint32_t own,
               std::uint32_t limit )
{
  Wrote const wrote = chunkAt( build, held.where, own - held.begin );
  std::uint32_t const end = wrote.chunk < 0 ? held.end : held.begin + wrote.end;

  ByteRun run{ .at = at,
               .length = std::min( end - own, limit - address ),
               .address = address,
               .what = "section",
               .section = held.name,
               .module = held.module,
               .chunk = wrote.chunk,
               .source = {},

               .data = {} };
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
                std::optional<PhaseIndex> live = std::nullopt,
                std::optional<std::uint32_t> shown = std::nullopt )
{
  diag::SourceManager const& sources = build.sources();
  std::optional<PhaseIndex> const entry = live.has_value() ? live : std::optional{ build.project().phases.entry };
  std::uint32_t const last = address + length;

  while ( address < last )
  {
    std::uint32_t const offsetInFile = at + ( address - ( last - length ) );
    Standing const standing =
        containerWrote( address ).empty() ? sectionAt( build, map, entry, address, shown ) : Standing{};

    if ( standing.held == nullptr )
    {
      std::uint32_t runEnd = address;
      std::string_view const name = outsideName( address );
      while ( runEnd < last && outsideName( runEnd ) == name &&
              ( !containerWrote( runEnd ).empty() || sectionAt( build, map, entry, runEnd, shown ).held == nullptr ) )
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
                              .source = {},

                              .data = {} } );
      accounted += runEnd - address;
      address = runEnd;
      continue;
    }

    ByteRun const run = runOf( build, *standing.held, sources, offsetInFile, address, standing.at, last );
    accounted += run.length;
    address += run.length;
    out.push_back( run );
  }
}

// Where a sector begins in the file, the header before it.
std::uint32_t offsetOfSector( std::uint32_t sector )
{
  return ATR_HEADER_SIZE + atrOffsetOfSector( sector );
}

/// Where a segment carries a stored form the load decodes: from the address
/// the cells named up to the cells, where the two are one segment, or to the
/// segment's end, and the format.
struct Encoded
{
  std::uint32_t begin = 0;
  std::uint32_t end = 0;
  std::string format;
};

/// The forms in a stream of segments, known by what follows each: the cells
/// that say where it stands, then INITAD at a decoder's glue — see
/// docs/decisions/0235-a-xex-decodes-in-place-while-it-loads.md and
/// docs/decisions/0259-a-xex-may-be-compressed-segment-by-segment.md. One
/// entry per segment, read before any is attributed, since the segment that
/// says a form is one comes after it.
std::vector<std::optional<Encoded>>
encodedIn( Patched const& build, std::span<std::uint8_t const> bytes, std::size_t at )
{
  struct Seen
  {
    std::uint32_t start = 0;
    std::uint32_t end = 0; ///< Inclusive, as the format has it.
    std::size_t data = 0;
  };

  std::vector<Seen> seen;
  while ( at + 4 <= bytes.size() && wordAt( bytes, at ) != 0xFFFF )
  {
    Seen const one{ .start = wordAt( bytes, at ), .end = wordAt( bytes, at + 2 ), .data = at + 4 };
    std::uint32_t const length = one.end >= one.start ? one.end - one.start + 1 : 0;
    if ( one.data + length > bytes.size() )
    {
      break;
    }
    seen.push_back( one );
    at = one.data + length;
  }
  std::vector<std::optional<Encoded>> found( seen.size() );

  GlobalSymbols const& symbols = build.symbols();
  std::optional<std::uint32_t> const cells =
      addressOfExported( symbols, UNPACK_FROM_NAME, build.sizes(), build.layout() );
  if ( !cells.has_value() )
  {
    return found;
  }
  auto const covers = []( Seen const& one, std::uint32_t address )
  { return one.start <= address && address + 1 <= one.end; };
  std::optional<std::uint32_t> named;
  for ( std::size_t index = 0; index < seen.size(); ++index )
  {
    if ( covers( seen[index], *cells ) )
    {
      named = wordAt( bytes, seen[index].data + ( *cells - seen[index].start ) );
    }
    if ( !covers( seen[index], INITAD ) || !named.has_value() )
    {
      continue;
    }
    std::uint32_t const called = wordAt( bytes, seen[index].data + ( INITAD - seen[index].start ) );
    std::string format;
    for ( Format const& one : formats() )
    {
      for ( std::string const& glue : { loadGlueName( one.name ), underGlueName( one.name ) } )
      {
        if ( addressOfExported( symbols, glue, build.sizes(), build.layout() ) == called )
        {
          format = one.name;
        }
      }
    }
    if ( format.empty() )
    {
      continue;
    }
    for ( std::size_t back = index; back-- > 0; )
    {
      if ( seen[back].start <= *named && *named <= seen[back].end )
      {
        std::uint32_t const end = *cells > *named && *cells <= seen[back].end ? *cells : seen[back].end + 1;
        found[back] = Encoded{ .begin = *named, .end = end, .format = format };
        break;
      }
    }
    named.reset();
  }
  return found;
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
  // The state the load last named, for the units and named states written
  // through the Window: what a byte there is depends on which of them the
  // driver was showing.
  Project const& project = build.project();
  std::optional<std::uint32_t> const cell =
      addressOfExported( build.symbols(), LOAD_UNIT_NAME, build.sizes(), build.layout() );
  bool const wide = project.driver.has_value() && project.driver->stream.has_value() &&
                    stateBytesOf( project, *project.driver->stream ) > 1;
  std::optional<std::uint32_t> shown;
  std::vector<std::optional<Encoded>> const encoded = encodedIn( build, bytes, at );
  for ( std::size_t index = 0; at + 4 <= bytes.size(); ++index )
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
                                       .source = {},

                                       .data = {} } );
      return accounted + 2;
    }

    at += 4;
    accounted += 4;

    std::uint32_t const length = segment.end >= segment.start ? segment.end - segment.start + 1 : 0;
    if ( at + length > bytes.size() )
    {
      break;
    }

    // A stored form is what the load decodes, and not the bytes of whatever
    // Section stands where the loader put it until the decode is done.
    std::optional<Encoded> const form = index < encoded.size() ? encoded[index] : std::nullopt;
    std::uint32_t const formBegin = form.has_value() ? form->begin : segment.start + length;
    std::uint32_t const formEnd = form.has_value() ? form->end : segment.start + length;
    attribute( build,
               map,
               static_cast<std::uint32_t>( at ),
               segment.start,
               formBegin - segment.start,
               segment.runs,
               accounted,
               std::nullopt,
               shown );
    if ( form.has_value() )
    {
      segment.runs.push_back( ByteRun{ .at = static_cast<std::uint32_t>( at ) + ( formBegin - segment.start ),
                                       .length = formEnd - formBegin,
                                       .address = formBegin,
                                       .what = "encoded, " + form->format,
                                       .section = {},
                                       .module = {},
                                       .chunk = -1,
                                       .source = {},

                                       .data = {} } );
      accounted += formEnd - formBegin;
      attribute( build,
                 map,
                 static_cast<std::uint32_t>( at ) + ( formEnd - segment.start ),
                 formEnd,
                 segment.start + length - formEnd,
                 segment.runs,
                 accounted,
                 std::nullopt,
                 shown );
    }
    if ( cell.has_value() && segment.start <= *cell && *cell + ( wide ? 1 : 0 ) <= segment.end )
    {
      std::size_t const value = at + ( *cell - segment.start );
      shown = bytes[value] | ( wide ? static_cast<std::uint32_t>( bytes[value + 1] ) << 8 : 0 );
    }
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
  std::uint32_t const header = CAR_HEADER_SIZE;
  std::uint32_t accounted = header;
  facts.header.push_back( ByteRun{ .at = 0,
                                   .length = header,
                                   .address = 0,
                                   .what = "container header",
                                   .section = {},
                                   .module = {},
                                   .chunk = -1,
                                   .source = {},

                                   .data = {} } );

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
  for ( MapEntry const& held : map.entries )
  {
    // A run for each form the Payload came to: one unless no unit of storage
    // held the whole of it, and then they are placed apart.
    for ( MapPlacement const& piece : held.waits )
    {
      if ( piece.size == 0 )
      {
        continue;
      }
      stored.push_back( ByteRun{ .at = header + ( piece.at.bank.value * map.bankSize ) + piece.at.offset,
                                 .length = piece.size,
                                 .address = 0,
                                 .what = held.transform.empty() ? "payload" : "payload, " + held.transform,
                                 .section = held.name,
                                 .module = held.module,
                                 .chunk = -1,
                                 .source = {},

                                 .data = {} } );
    }
    if ( !held.waits.empty() )
    {
      // It waits in storage, so it is not also a Pane's.
      continue;
    }
    // A Pane's Section stands in its Bank at its own offset within its
    // Window, a copy in each member of a family: where storage put it, which
    // is where the Container wrote it and where it is read from.
    if ( held.pane.empty() )
    {
      continue;
    }
    InitialisedExtent const extent = build.sizes().initialisedExtentOf( held.where );
    for ( Storage::PaneImage const& image : build.storage().paneImages() )
    {
      if ( image.where != held.where )
      {
        continue;
      }
      stored.push_back( ByteRun{ .at = header + target.positionOf( image.at ),
                                 .length = extent.size(),
                                 .address = held.begin + extent.begin,
                                 .what = "pane",
                                 .section = held.name,
                                 .module = held.module,
                                 .chunk = -1,
                                 .source = {},

                                 .data = {} } );
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
                               .source = {},

                               .data = {} } );
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
                                 .source = {},

                                 .data = {} } );
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
                               .source = {},

                               .data = {} } );
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
  std::uint32_t accounted = ATR_HEADER_SIZE;
  facts.header.push_back( ByteRun{ .at = 0,
                                   .length = ATR_HEADER_SIZE,
                                   .address = 0,
                                   .what = "container header",
                                   .section = {},
                                   .module = {},
                                   .chunk = -1,
                                   .source = {},

                                   .data = {} } );

  // The record is the Sections of `nga.boot` at the addresses their pins gave
  // them, read into `$0700` as three sectors of 128 bytes.
  ContainerSegment boot;
  boot.at = ATR_HEADER_SIZE;
  boot.start = ATR_BOOT_ADDRESS;
  boot.end = ATR_BOOT_ADDRESS + ATR_BOOT_SIZE - 1;
  attribute( build, map, ATR_HEADER_SIZE, ATR_BOOT_ADDRESS, ATR_BOOT_SIZE, boot.runs, accounted );
  facts.segments.push_back( std::move( boot ) );

  // Where the image begins, taken from the record where the Container patched
  // it: `ngaBootImage` holds the sector as two bytes.
  std::uint32_t imageSector = 0;
  for ( MapEntry const& held : map.entries )
  {
    if ( held.name == BOOT_IMAGE_NAME && held.begin >= ATR_BOOT_ADDRESS )
    {
      std::size_t const where = ATR_HEADER_SIZE + ( held.begin - ATR_BOOT_ADDRESS );
      if ( where + 1 < bytes.size() )
      {
        imageSector = wordAt( bytes, where );
      }
      break;
    }
  }

  std::uint32_t const storageAt = offsetOfSector( ATR_FIRST_STORAGE_SECTOR );
  std::uint32_t const imageAt = imageSector >= ATR_FIRST_STORAGE_SECTOR ? offsetOfSector( imageSector )
                                                                        : static_cast<std::uint32_t>( bytes.size() );

  // Storage: every Payload and every Frame at the position PlaceStorage gave
  // it, end to end from sector four. A unit is 65536 bytes, which is what
  // makes the model's three-byte address the hardware's.
  std::vector<ByteRun> stored;
  for ( MapEntry const& held : map.entries )
  {
    // A run for each form the Payload came to: one unless no unit of storage
    // held the whole of it, and then they are placed apart.
    for ( MapPlacement const& piece : held.waits )
    {
      if ( piece.size == 0 )
      {
        continue;
      }
      std::uint32_t const at = storageAt + ( piece.at.bank.value * map.bankSize ) + piece.at.offset;
      stored.push_back( ByteRun{ .at = at,
                                 .length = piece.size,
                                 .address = held.begin,
                                 .what = held.transform.empty() ? "payload" : "payload, " + held.transform,
                                 .section = held.name,
                                 .module = held.module,
                                 .chunk = -1,
                                 .source = {},

                                 .data = {} } );
    }
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
                               .source = {},

                               .data = {} } );
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
                               .source = {},

                               .data = {} } );
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
                             .source = {},

                             .data = {} } );
  }
  for ( ByteRun const& gap : gaps )
  {
    accounted += gap.length;
  }
  facts.header.insert( facts.header.end(), gaps.begin(), gaps.end() );

  facts.unaccounted = facts.size - std::min( accounted, facts.size );
  return facts;
}

namespace
{

/// The Container read back, every run named and no byte's value copied.
ContainerFacts
readBack( Patched const& build, MemoryMap const& map, std::span<std::uint8_t const> bytes, Container container )
{
  ContainerFacts facts;
  facts.container = std::string{ nameOf( container ) };
  facts.size = static_cast<std::uint32_t>( bytes.size() );

  if ( bytes.size() < 2 )
  {
    facts.unaccounted = facts.size;
    return facts;
  }

  switch ( container )
  {
  case Container::XEX:
    facts.header.push_back( ByteRun{ .at = 0,
                                     .length = 2,
                                     .address = 0,
                                     .what = "container header",
                                     .section = {},
                                     .module = {},
                                     .chunk = -1,
                                     .source = {},

                                     .data = {} } );
    facts.unaccounted = facts.size - std::min( 2 + readSegments( build, map, bytes, 2, facts ), facts.size );
    return facts;
  case Container::ATR:
    return diskette( build, map, bytes, std::move( facts ) );
  case Container::CAR:
  {
    if ( std::optional<Cartridge> const& board = build.project().cartridge; board.has_value() )
    {
      return cartridge( build, map, bytes, std::move( facts ), *board );
    }
    break;
  }
  // Read back by nothing yet: their bytes are claimed by nothing.
  case Container::RAW_IMAGE:
  case Container::BS93:
  case Container::LNX:
  case Container::LYX:
  case Container::PRG:
  case Container::D64:
  case Container::D80:
  case Container::D82:
    break;
  }

  facts.unaccounted = facts.size;
  return facts;
}

} // namespace

ContainerFacts
containerFactsOf( Patched const& build, MemoryMap const& map, std::span<std::uint8_t const> bytes, Container container )
{
  ContainerFacts facts = readBack( build, map, bytes, container );

  // A statement's own bytes, which a listing shows beside it; what the
  // Container put there for itself is named for what it is and needs none.
  auto const copy = [bytes]( std::vector<ByteRun>& runs )
  {
    for ( ByteRun& run : runs )
    {
      if ( run.what == "section" && run.at + run.length <= bytes.size() )
      {
        auto const from = bytes.begin() + run.at;
        run.data.assign( from, from + run.length );
      }
    }
  };
  copy( facts.header );
  for ( ContainerSegment& segment : facts.segments )
  {
    copy( segment.runs );
  }
  return facts;
}

} // namespace nga::model
