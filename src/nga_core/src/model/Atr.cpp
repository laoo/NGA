#include "nga/model/Emit.hpp"

#include "nga/model/Atr.hpp"
#include "nga/model/Segments.hpp"
#include "nga/model/Transition.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace nga::model
{

namespace
{

/// The sixteen bytes in front of a diskette's sectors: the magic every reader
/// knows it by, the size of what follows in paragraphs of sixteen bytes — a
/// third byte of it after the sector size, since a large image needs more than
/// sixteen — what one sector holds, and a flags byte that is zero for an image
/// nothing write-protects.
///
/// The size is the file's own and not `sectors * ATR_SECTOR_SIZE`, because the
/// first three sectors hold 128 bytes: a reader tells a double-density image
/// with short boot sectors from one padded to 256 by whether the paragraph
/// count divides by sixteen, and this one does not. See docs/spec/atr.md.
std::array<std::uint8_t, 16> atrHeader( std::uint32_t length )
{
  std::uint32_t const paragraphs = length / 16;
  std::array<std::uint8_t, 16> header{};
  header[0] = 0x96;
  header[1] = 0x02;
  header[2] = static_cast<std::uint8_t>( paragraphs & 0xFF );
  header[3] = static_cast<std::uint8_t>( ( paragraphs >> 8 ) & 0xFF );
  header[4] = static_cast<std::uint8_t>( ATR_SECTOR_SIZE & 0xFF );
  header[5] = static_cast<std::uint8_t>( ( ATR_SECTOR_SIZE >> 8 ) & 0xFF );
  header[6] = static_cast<std::uint8_t>( ( paragraphs >> 16 ) & 0xFF );
  return header;
}

/// The Module holding the boot record, which the tool added itself.
std::optional<ModuleIndex> bootModuleOf( Project const& project )
{
  for ( std::uint32_t index = 0; index < project.modules.size(); ++index )
  {
    if ( project.modules[index].name == BOOT_MODULE )
    {
      return ModuleIndex{ index };
    }
  }
  return std::nullopt;
}

/// What the Project asks of the diskette, said before a byte is written.
/// A unit set is Banks, which the boot record cannot show; a unit that is not
/// a sector is arithmetic neither the Container nor the driver does; a driver
/// that streams through a Window is one for a mapped medium.
bool storageIsSectors( Patched const& build, diag::DiagnosticSink& sink )
{
  Project const& project = build.project();
  Target const& target = build.target();
  bool ok = true;
  if ( target.storageUnits.has_value() )
  {
    sink.add( diag::diagnostic( diag::DiagnosticId::ATR_STORAGE_IS_A_UNIT_SET )
                  .arg( "name", target.unitSets[target.storageUnits->value].name )
                  .sortedBy( "storage" ) );
    ok = false;
  }
  else if ( target.unitCount > 0 && target.unitSize != ATR_UNIT_SIZE )
  {
    sink.add( diag::diagnostic( diag::DiagnosticId::ATR_UNIT_IS_NOT_SECTORS )
                  .arg( "sector", ATR_SECTOR_SIZE )
                  .arg( "size", ATR_UNIT_SIZE )
                  .arg( "unit", target.unitSize )
                  .sortedBy( "storage" ) );
    ok = false;
  }
  if ( project.driver.has_value() && project.driver->stream.has_value() )
  {
    sink.add( diag::diagnostic( diag::DiagnosticId::ATR_DRIVER_NAMES_A_WINDOW )
                  .arg( "name", target.windows[project.driver->stream->value].name )
                  .sortedBy( "driver" ) );
    ok = false;
  }
  return ok;
}

/// Nothing of the program may stand in the boot record: the loader is reading
/// the image from there when a load into those bytes would happen.
void reportInRecord( diag::DiagnosticSink& sink, Patched const& build, Resident const& one )
{
  GlobalSymbols const& symbols = build.symbols();
  Section const& section = symbols.moduleAt( one.where.module ).sectionAt( one.where.section );
  sink.add(
      diag::diagnostic( diag::DiagnosticId::ATR_SECTION_IN_BOOT_RECORD )
          .at( section.span().begin, section.span().length )
          .arg( "section", symbols.moduleAt( one.where.module ).displayNameOf( one.where.section, build.sources() ) )
          .arg( "address", static_cast<std::int64_t>( one.address ) )
          .arg( "begin", static_cast<std::int64_t>( ATR_BOOT_ADDRESS ) )
          .arg( "end", static_cast<std::int64_t>( ATR_BOOT_ADDRESS + ATR_BOOT_SIZE - 1 ) ) );
}

} // namespace

AtrFile emitAtr( Patched const& build, diag::DiagnosticSink& sink )
{
  Project const& project = build.project();
  Target const& target = build.target();
  GlobalSymbols const& symbols = build.symbols();
  Sizes const& sizes = build.sizes();
  Layout const& layout = build.layout();

  std::optional<ModuleIndex> const boot = bootModuleOf( project );
  if ( !boot.has_value() )
  {
    sink.add( diag::diagnostic( diag::DiagnosticId::ATR_WITHOUT_BOOT_RECORD ).sortedBy( "boot" ) );
    return {};
  }
  if ( !storageIsSectors( build, sink ) )
  {
    return {};
  }

  // Every Section that is in memory before the program runs, split by where
  // it stands: the boot record is what the OS reads into $0700 itself, and the
  // load image is everything else, which the loader in that record reads from
  // the sectors after storage.
  std::uint32_t const recordEnd = ATR_BOOT_ADDRESS + ATR_BOOT_SIZE;
  std::vector<std::uint8_t> record( ATR_BOOT_SIZE, 0 );
  std::vector<Resident> image;
  bool refused = false;
  for ( Resident const& one : residentSections( build, {} ) )
  {
    std::uint32_t const end = one.address + static_cast<std::uint32_t>( one.content.size() );
    if ( one.address >= recordEnd || end <= ATR_BOOT_ADDRESS )
    {
      image.push_back( one );
      continue;
    }
    if ( one.where.module.value != boot->value )
    {
      // The loader is still reading the image when a load into those bytes
      // would happen, so nothing of the program may stand there.
      reportInRecord( sink, build, one );
      refused = true;
      continue;
    }
    if ( one.address < ATR_BOOT_ADDRESS || end > recordEnd )
    {
      sink.add( diag::diagnostic( diag::DiagnosticId::ATR_BOOT_RECORD_TOO_LARGE )
                    .arg( "available", ATR_BOOT_SIZE )
                    .arg( "required", end - ATR_BOOT_ADDRESS )
                    .sortedBy( "boot" ) );
      return {};
    }
    std::ranges::copy( one.content, record.begin() + static_cast<std::ptrdiff_t>( one.address - ATR_BOOT_ADDRESS ) );
  }
  if ( refused )
  {
    return {};
  }

  // The load image: those Sections as the segments a `.xex` carries, and
  // `RUNAD` for the loader to take the program's start from. $FFFF closes it.
  SegmentWriter out;
  for ( Resident const& one : image )
  {
    out.segment( one.address, one.content );
  }

  diag::SeverityPolicy quietPolicy;
  diag::DiagnosticSink quiet{ quietPolicy };
  bool const alreadyReported = isEnteredByAnEdge( project.phases, project.phases.entry );
  std::optional<std::uint32_t> const run =
      entryAddressOf( build, project.phases.entry, alreadyReported ? quiet : sink );
  if ( !run.has_value() )
  {
    return {};
  }
  out.word( RUNAD, *run );
  std::vector<std::uint8_t> stream = std::move( out ).take();
  stream.push_back( 0xFF );
  stream.push_back( 0xFF );

  // Where everything stands on the diskette. Storage begins at a fixed sector
  // because the driver reaches a unit by number and nothing tells the driver
  // where storage is; the image begins after the storage the program came to
  // **use**, which is why its sector is written into the record here and not
  // into the loader's source — see docs/spec/atr.md.
  std::vector<Stored> const stored = storedImages( build );
  std::uint32_t used = 0;
  for ( Stored const& one : stored )
  {
    used = std::max( used, target.positionOf( one.at ) + static_cast<std::uint32_t>( one.form.size() ) );
  }
  std::uint32_t const storageSectors = ( used + ATR_SECTOR_SIZE - 1 ) / ATR_SECTOR_SIZE;
  std::uint32_t const imageSector = ATR_FIRST_STORAGE_SECTOR + storageSectors;
  std::uint32_t const imageSectors =
      ( static_cast<std::uint32_t>( stream.size() ) + ATR_SECTOR_SIZE - 1 ) / ATR_SECTOR_SIZE;
  std::uint32_t const required = imageSector - 1 + imageSectors;
  if ( required > ATR_MOST_SECTORS )
  {
    sink.add( diag::diagnostic( diag::DiagnosticId::ATR_DOES_NOT_FIT )
                  .arg( "available", ATR_MOST_SECTORS )
                  .arg( "required", required )
                  .sortedBy( "size" ) );
    return {};
  }

  std::optional<std::uint32_t> const bootImage = addressOfExported( symbols, BOOT_IMAGE_NAME, sizes, layout );
  if ( !bootImage.has_value() || *bootImage < ATR_BOOT_ADDRESS || *bootImage + 1 >= recordEnd )
  {
    sink.add( diag::diagnostic( diag::DiagnosticId::ATR_WITHOUT_BOOT_RECORD ).sortedBy( "boot" ) );
    return {};
  }
  std::size_t const at = *bootImage - ATR_BOOT_ADDRESS;
  record[at] = static_cast<std::uint8_t>( imageSector & 0xFF );
  record[at + 1] = static_cast<std::uint8_t>( ( imageSector >> 8 ) & 0xFF );

  // An ordinary program comes out as a diskette; one that needs more sectors
  // grows the image past one.
  std::uint32_t const count = std::max( ATR_DISKETTE_SECTORS, required );
  std::vector<std::uint8_t> sectors( atrOffsetOfSector( count + 1 ), 0 );
  std::ranges::copy( record, sectors.begin() );

  // Storage is one space end to end and a unit is 256 sectors, so a position
  // in it is a position on the diskette: an image that runs from one unit
  // into the next runs into the sector after, which is where the driver's
  // stream carries on.
  std::size_t const storageAt = atrOffsetOfSector( ATR_FIRST_STORAGE_SECTOR );
  for ( Stored const& one : stored )
  {
    std::ranges::copy( one.form,
                       sectors.begin() + static_cast<std::ptrdiff_t>( storageAt + target.positionOf( one.at ) ) );
  }
  std::ranges::copy( stream, sectors.begin() + static_cast<std::ptrdiff_t>( atrOffsetOfSector( imageSector ) ) );

  std::array<std::uint8_t, 16> const header = atrHeader( static_cast<std::uint32_t>( sectors.size() ) );
  std::vector<std::uint8_t> file( header.begin(), header.end() );
  file.insert( file.end(), sectors.begin(), sectors.end() );
  return AtrFile{ .bytes = std::move( file ) };
}

} // namespace nga::model
