#include "nga/model/Car.hpp"

#include "nga/model/Emit.hpp"
#include "nga/model/Segments.hpp"
#include "nga/model/Storage.hpp"
#include "nga/model/Transition.hpp"

#include <spdlog/fmt/fmt.h>

#include <algorithm>
#include <array>

namespace nga::model
{

namespace
{

constexpr std::uint32_t BANK = 0x2000;

/// A XEGS board: `banks` of eight kilobytes, the last of them fixed at
/// `$A000-$BFFF` and the rest switched into `$8000-$9FFF` by the byte written
/// to `$D5xx`. The fixed one is the last of the image, so what a Section
/// standing there is at in the file is the image less one bank.
constexpr Cartridge xegs( std::string_view name, CartridgeFormat format, std::uint32_t mapper, std::uint32_t banks )
{
  return Cartridge{ .name = name,
                    .format = format,
                    .mapper = mapper,
                    .imageSize = banks * BANK,
                    .fixed = AddressRange{ .begin = 0xA000, .end = 0xC000 },
                    .fixedOffset = ( banks - 1 ) * BANK,
                    .window = AddressRange{ .begin = 0x8000, .end = 0xA000 },
                    .units = banks - 1,
                    .bankSize = BANK };
}

constexpr std::array<Cartridge, 8> CARTRIDGES{ { Cartridge{ .name = "8k",
                                                            .format = CartridgeFormat::STANDARD_8K,
                                                            .mapper = 1,
                                                            .imageSize = 0x2000,
                                                            .fixed = AddressRange{ .begin = 0xA000, .end = 0xC000 },
                                                            .fixedOffset = 0,
                                                            .window = AddressRange{},
                                                            .units = 0,
                                                            .bankSize = 0 },
                                                 Cartridge{ .name = "16k",
                                                            .format = CartridgeFormat::STANDARD_16K,
                                                            .mapper = 2,
                                                            .imageSize = 0x4000,
                                                            .fixed = AddressRange{ .begin = 0x8000, .end = 0xC000 },
                                                            .fixedOffset = 0,
                                                            .window = AddressRange{},
                                                            .units = 0,
                                                            .bankSize = 0 },
                                                 xegs( "xegs32", CartridgeFormat::XEGS_32K, 12, 4 ),
                                                 xegs( "xegs64", CartridgeFormat::XEGS_64K, 13, 8 ),
                                                 xegs( "xegs128", CartridgeFormat::XEGS_128K, 14, 16 ),
                                                 xegs( "xegs256", CartridgeFormat::XEGS_256K, 23, 32 ),
                                                 xegs( "xegs512", CartridgeFormat::XEGS_512K, 24, 64 ),
                                                 xegs( "xegs1024", CartridgeFormat::XEGS_1024K, 25, 128 ) } };

std::string rangeText( AddressRange range )
{
  return fmt::format( "${:04X}..${:04X}", range.begin, range.end - 1 );
}

/// The one `rom` Region of the Target, or nothing where there is none or
/// several: a board has one fixed part and the Variant says where it is.
std::optional<Region> romRegionOf( Target const& target )
{
  std::optional<Region> found;
  for ( Region const& region : target.regions )
  {
    if ( region.property != RegionProperty::ROM )
    {
      continue;
    }
    if ( found.has_value() )
    {
      return std::nullopt;
    }
    found = region;
  }
  return found;
}

/// The six bytes the OS reads a cartridge from, as the assembler reads them.
/// What it does with each is docs/spec/car.md.
constexpr std::string_view CART_HEADER_SOURCE =
    R"asm(; The cartridge header: the six bytes at $BFFA the OS reads before it runs
; anything of a cartridge. $BFFC being zero is what says one is there, and the
; OS then tests that $BFFB cannot be written, which it cannot, being ROM. See
; docs/spec/car.md.

.export ngaCartStart
.section absolute at $BFFA, root, readonly
ngaCartStart
        .word 0                 ; the program's start, which the Container
                                ; patches once addresses exist: the OS jumps
                                ; through it after E: is open on IOCB 0, and
                                ; again on every reset
        .byte 0                 ; $BFFC: a cartridge is here
        .byte $04               ; $BFFD: started through $BFFA, no disk boot,
                                ; and not the diagnostic cartridge bit 7 would
                                ; make it
        .root
        .word ngaCartInit       ; $BFFE: called in the middle of the cold start
.ends

; The init vector points here and here does nothing on purpose: it is called
; before the OS has finished its own cold start and before E: exists, so
; anything run there would run in a machine half set up.
.proc ngaCartInit, root
        rts
.endp
)asm";

} // namespace

std::string cartHeaderSource()
{
  return std::string{ CART_HEADER_SOURCE };
}

void addCartHeader( Project& project, diag::SourceManager& sources )
{
  diag::FileId const file = sources.addFile( "<nga>/cart.asm", cartHeaderSource() );
  ModuleIndex const index{ static_cast<std::uint32_t>( project.modules.size() ) };
  project.modules.push_back( ProjectModule{
      .name = std::string{ CART_MODULE }, .file = file, .residency = {}, .generated = Generated::NONE } );
  for ( Phase& phase : project.phases.phases )
  {
    phase.needs.push_back( index );
  }
  deriveResidency( project );
}

std::span<Cartridge const> cartridges()
{
  return CARTRIDGES;
}

std::optional<Cartridge> cartridgeNamed( std::string_view name )
{
  auto const* const found = std::ranges::find( CARTRIDGES, name, &Cartridge::name );
  if ( found == CARTRIDGES.end() )
  {
    return std::nullopt;
  }
  return *found;
}

std::string knownCartridges()
{
  std::string known;
  for ( Cartridge const& one : CARTRIDGES )
  {
    known += known.empty() ? "" : ", ";
    known += fmt::format( "`{}`", one.name );
  }
  return known;
}

CarFile emitCar( Patched const& build, diag::DiagnosticSink& sink )
{
  Project const& project = build.project();
  GlobalSymbols const& symbols = build.symbols();
  Sizes const& sizes = build.sizes();
  Layout const& layout = build.layout();

  // A Project whose Container is a `.car` has a board: one that named none was
  // refused where the document was read, and an error there stops the build
  // long before this.
  std::optional<Cartridge> const board =
      project.cartridge.has_value() ? cartridgeNamed( *project.cartridge ) : std::nullopt;
  if ( !board.has_value() )
  {
    return {};
  }

  // What no Section accounts for is what an erased device holds, so an image
  // burnt onto one differs from it only where the program is.
  std::vector<std::uint8_t> image( board->imageSize, 0xFF );

  // The units, where the board switches them in. A unit is the window, so a
  // position in storage counted end to end **is** the offset in the image: the
  // Bank a position falls in is its offset divided by the window, which is what
  // the board wires the address lines to. An image that runs from one unit into
  // the next runs into the bank after, which is where the driver's stream
  // carries on.
  for ( Stored const& one : storedImages( build ) )
  {
    std::uint32_t const at = build.target().positionOf( one.at );
    if ( at + one.form.size() > image.size() )
    {
      sink.add( diag::diagnostic( diag::DiagnosticId::CAR_STORAGE_PAST_THE_END )
                    .arg( "name", std::string{ board->name } )
                    .arg( "available", static_cast<std::int64_t>( image.size() ) )
                    .arg( "required", static_cast<std::int64_t>( at + one.form.size() ) )
                    .sortedBy( "storage" ) );
      return {};
    }
    std::ranges::copy( one.form, image.begin() + static_cast<std::ptrdiff_t>( at ) );
  }

  bool refused = false;
  for ( std::uint32_t module = 0; module < symbols.modules().size(); ++module )
  {
    Module const& one = symbols.modules()[module];
    for ( std::uint32_t index = 0; index < one.sections().size(); ++index )
    {
      SectionRef const where{ .module = ModuleIndex{ module }, .section = SectionIndex{ index } };
      Section const& section = one.sectionAt( where.section );
      if ( !layout.isPlaced( where ) || !sizes.isKnown( where ) || sizes.sizeOfSection( where ) == 0 ||
           !section.emitsBytes() )
      {
        continue;
      }
      std::uint32_t const address = layout.addressOf( where );
      std::uint32_t const size = sizes.sizeOfSection( where );

      // A Pane's Section stands in a Bank and a Payload waits in one, and both
      // are written above: what storage holds is one space and the writer
      // knows it by position, not by address.
      if ( section.pane().has_value() || build.storage().hasPayload( where ) )
      {
        continue;
      }

      // Everything else with bytes is in the fixed part or is nowhere: nothing
      // loads a cartridge, so an address outside it is one no byte of the
      // image ever reaches.
      if ( address < board->fixed.begin || address + size > board->fixed.end )
      {
        sink.add( diag::diagnostic( diag::DiagnosticId::CAR_SECTION_OUTSIDE_ROM )
                      .at( section.span().begin, section.span().length )
                      .arg( "section", one.displayNameOf( where.section, build.sources() ) )
                      .arg( "address", static_cast<std::int64_t>( address ) )
                      .arg( "name", std::string{ board->name } )
                      .arg( "range", rangeText( board->fixed ) ) );
        refused = true;
        continue;
      }
      std::span<std::uint8_t const> const content = build.bytes().of( where );
      std::uint32_t const at = board->fixedOffset + ( address - board->fixed.begin );
      for ( std::uint32_t offset = 0; offset < size && offset < content.size(); ++offset )
      {
        image[at + offset] = content[offset];
      }
    }
  }
  if ( refused )
  {
    return {};
  }

  // Where the cold start's Frame waits, into the three bytes its stub reads:
  // nothing knows them until every Payload has been placed, which is why they
  // are patched here and not written into the Module's own text.
  std::optional<FrameIndex> const cold = build.storage().frameOf( std::nullopt, project.phases.entry );
  std::optional<std::uint32_t> const coldAt = addressOfExported( symbols, COLD_FRAME_NAME, sizes, layout );
  if ( cold.has_value() != coldAt.has_value() )
  {
    sink.add( diag::diagnostic( diag::DiagnosticId::CAR_WITHOUT_HEADER ).sortedBy( "header" ) );
    return {};
  }
  if ( cold.has_value() )
  {
    StorageAddress const at = build.storage().frameAddressOf( *cold );
    std::uint32_t const where = board->fixedOffset + ( *coldAt - board->fixed.begin );
    image[where] = static_cast<std::uint8_t>( at.bank.value );
    image[where + 1] = static_cast<std::uint8_t>( at.offset & 0xFF );
    image[where + 2] = static_cast<std::uint8_t>( ( at.offset >> 8 ) & 0xFF );
  }

  // What the OS jumps through: the stub that hands the cold start's Frame to
  // the routine, where there is one, and the entry Phase's entry Label
  // otherwise. When some edge enters the entry Phase, Patch has resolved its
  // entry for a table and reported whatever was wrong with it.
  diag::SeverityPolicy quietPolicy;
  diag::DiagnosticSink quiet{ quietPolicy };
  bool const alreadyReported = isEnteredByAnEdge( project.phases, project.phases.entry );
  std::optional<std::uint32_t> const stub = addressOfExported( symbols, COLD_START_NAME, sizes, layout );
  std::optional<std::uint32_t> const run =
      stub.has_value() ? stub : entryAddressOf( build, project.phases.entry, alreadyReported ? quiet : sink );
  std::optional<std::uint32_t> const start = addressOfExported( symbols, CART_START_NAME, sizes, layout );
  if ( !run.has_value() || !start.has_value() || *start < board->fixed.begin || *start + 1 >= board->fixed.end )
  {
    sink.add( diag::diagnostic( diag::DiagnosticId::CAR_WITHOUT_HEADER ).sortedBy( "header" ) );
    return {};
  }
  std::uint32_t const vector = board->fixedOffset + ( *start - board->fixed.begin );
  image[vector] = static_cast<std::uint8_t>( *run & 0xFF );
  image[vector + 1] = static_cast<std::uint8_t>( ( *run >> 8 ) & 0xFF );

  // `CART`, the mapper number and the sum of the image's bytes, both big
  // endian, and four bytes of nothing.
  std::uint32_t sum = 0;
  for ( std::uint8_t const byte : image )
  {
    sum += byte;
  }
  std::vector<std::uint8_t> file{ 'C', 'A', 'R', 'T' };
  for ( std::uint32_t const value : { board->mapper, sum } )
  {
    file.push_back( static_cast<std::uint8_t>( ( value >> 24 ) & 0xFF ) );
    file.push_back( static_cast<std::uint8_t>( ( value >> 16 ) & 0xFF ) );
    file.push_back( static_cast<std::uint8_t>( ( value >> 8 ) & 0xFF ) );
    file.push_back( static_cast<std::uint8_t>( value & 0xFF ) );
  }
  file.resize( 16, 0 );
  file.insert( file.end(), image.begin(), image.end() );
  return CarFile{ .bytes = std::move( file ) };
}

void checkCartridgeAgainstTarget( Cartridge const& board,
                                  Target const& target,
                                  diag::SourceSpan where,
                                  diag::DiagnosticSink& sink )
{
  // A Target that describes no machine describes no cartridge either: a
  // regression case writing a `target` block inline is a stand-in, and a check
  // earns teeth only where somebody wrote hardware truth down — the rule
  // docs/decisions/0179-a-container-is-chosen-in-the-project.md set for the
  // list of Containers.
  if ( target.regions.empty() )
  {
    return;
  }

  std::optional<Region> const rom = romRegionOf( target );
  if ( !rom.has_value() || rom->range.begin != board.fixed.begin || rom->range.end != board.fixed.end )
  {
    diag::Diagnostic finding = diag::diagnostic( diag::DiagnosticId::CARTRIDGE_FIXED_PART )
                                   .at( where.begin, where.length )
                                   .arg( "name", std::string{ board.name } )
                                   .arg( "range", rangeText( board.fixed ) )
                                   .arg( "found", rom.has_value() ? rangeText( rom->range ) : "no `rom` region" );
    if ( rom.has_value() && rom->site.has_value() )
    {
      finding = std::move( finding ).note( diag::diagnostic( diag::DiagnosticId::REGION_DECLARED_HERE )
                                               .at( rom->site->begin, rom->site->length )
                                               .arg( "region", displayNameOf( *rom ) ) );
    }
    sink.add( std::move( finding ) );
  }

  if ( !board.isBanked() )
  {
    return;
  }

  // The switched part, which is a Window of one range showing a unit set of
  // one Bank fewer than the board has.
  std::optional<WindowIndex> over;
  for ( std::uint32_t index = 0; index < target.windows.size(); ++index )
  {
    Window const& window = target.windows[index];
    if ( window.ranges.size() == 1 && window.ranges.front().begin == board.window.begin &&
         window.ranges.front().end == board.window.end )
    {
      over = WindowIndex{ index };
    }
  }
  if ( !over.has_value() )
  {
    sink.add( diag::diagnostic( diag::DiagnosticId::CARTRIDGE_WINDOW )
                  .at( where.begin, where.length )
                  .arg( "name", std::string{ board.name } )
                  .arg( "range", rangeText( board.window ) ) );
    return;
  }

  std::optional<UnitSetIndex> const set = target.unitSetShownBy( *over );
  std::uint32_t const count = set.has_value() ? target.unitSets[set->value].count : 0;
  if ( count != board.units )
  {
    sink.add( diag::diagnostic( diag::DiagnosticId::CARTRIDGE_UNITS )
                  .at( where.begin, where.length )
                  .arg( "name", std::string{ board.name } )
                  .arg( "window", target.windows[over->value].name )
                  .arg( "units", board.units )
                  .arg( "found", count ) );
  }
}

} // namespace nga::model
