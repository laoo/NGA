#include "nga/model/Emit.hpp"

#include "nga/model/CbmDisk.hpp"
#include "nga/model/Segments.hpp"
#include "nga/model/Storage.hpp"

#include <algorithm>

namespace nga::model
{

namespace
{

/// A sector, as the writer hands them about.
struct Sector
{
  std::uint32_t track = 0;
  std::uint32_t sector = 0;
};

/// What a CBM directory calls a program, closed: `$82` is `PRG` with the bit
/// that says the file was written to the end.
constexpr std::uint8_t CLOSED_PRG = 0x82;

/// What fills a name and the fields around it: `$A0` is the shifted space a
/// CBM directory pads with, and what a DOS stops comparing at.
constexpr std::uint8_t PAD = 0xA0;

/// The name the tool gives the one file it writes, in the code a PET sends
/// when somebody types it. `LOAD "NGA",8` puts `$4E $47 $41` on the bus, so
/// that is what the entry holds — **not** the `$C1`-and-up half of PETSCII,
/// which some tools write a name into and which a typed name never matches.
constexpr std::string_view PROGRAM_NAME = "NGA";

/// The map entry for a track: the count of free sectors and then the bitmap,
/// wherever this disk keeps it. A 1541 has them all in the header sector, four
/// bytes each from offset four; an 8050 splits them over sectors of their own,
/// five bytes each from offset six, each sector saying which tracks it covers.
std::span<std::uint8_t> bamEntryFor( std::span<std::uint8_t> image, CbmGeometry const& disk, std::uint32_t track )
{
  for ( CbmBamSector const& part : disk.bam )
  {
    if ( track < part.firstTrack || track > part.lastTrack )
    {
      continue;
    }
    std::size_t const at = cbmOffsetOfSector( disk, disk.bamTrack, part.sector );
    std::size_t const entry = disk.bamSharesHeader ? 4 + ( ( track - 1 ) * ( disk.bitmapBytes + 1 ) )
                                                   : 6 + ( ( track - part.firstTrack ) * ( disk.bitmapBytes + 1 ) );
    return image.subspan( at + entry, disk.bitmapBytes + 1 );
  }
  return {};
}

bool isFree( std::span<std::uint8_t> image, CbmGeometry const& disk, std::uint32_t track, std::uint32_t sector )
{
  std::span<std::uint8_t> const entry = bamEntryFor( image, disk, track );
  auto const bit = static_cast<std::uint8_t>( 1U << ( sector % 8 ) );
  return !entry.empty() && ( entry[1 + ( sector / 8 )] & bit ) != 0;
}

void markUsed( std::span<std::uint8_t> image, CbmGeometry const& disk, std::uint32_t track, std::uint32_t sector )
{
  std::span<std::uint8_t> const entry = bamEntryFor( image, disk, track );
  if ( entry.empty() )
  {
    return;
  }
  auto const bit = static_cast<std::uint8_t>( 1U << ( sector % 8 ) );
  std::size_t const byte = 1 + ( sector / 8 );
  if ( ( entry[byte] & bit ) != 0 )
  {
    entry[byte] = static_cast<std::uint8_t>( entry[byte] & ~bit );
    entry[0] = static_cast<std::uint8_t>( entry[0] - 1 );
  }
}

/// The disk's name, its identifier and the type of its DOS, at the offsets the
/// two layouts put them: a 1541 keeps them high in the header sector, above
/// the map that shares it, and an 8050 keeps them right behind the four bytes
/// of its own header.
void writeName( std::span<std::uint8_t> header, CbmGeometry const& disk )
{
  std::size_t const at = disk.bamSharesHeader ? 0x90 : 0x06;
  std::ranges::fill( header.subspan( at, 0x1B ), PAD );
  for ( std::size_t index = 0; index < PROGRAM_NAME.size(); ++index )
  {
    header[at + index] = static_cast<std::uint8_t>( PROGRAM_NAME[index] );
  }
  header[at + 0x12] = '6';
  header[at + 0x13] = '4';
  header[at + 0x15] = static_cast<std::uint8_t>( disk.dosType[0] );
  header[at + 0x16] = static_cast<std::uint8_t>( disk.dosType[1] );
}

} // namespace

CbmDiskFile emitCbmDisk( Patched const& build, CbmGeometry const& disk, diag::DiagnosticSink& sink )
{
  Target const& target = build.target();

  // The program itself, which is a `.prg` whatever it is written into: one
  // block and the line of BASIC that enters it. A diskette adds a file system
  // around it and a place for the Phases to wait, and nothing else.
  PrgFile const program = prgWith( build, sink );
  if ( sink.hasErrors() || program.bytes.empty() )
  {
    return CbmDiskFile{};
  }

  CbmDiskFile file;
  file.bytes.assign( disk.imageSize(), 0 );
  std::span<std::uint8_t> const image{ file.bytes };

  // Every sector free, and then taken as it is spoken for. The bitmap of a
  // track the surface does not have that many sectors on is left at zero.
  for ( CbmBamSector const& part : disk.bam )
  {
    std::span<std::uint8_t> const sector =
        image.subspan( cbmOffsetOfSector( disk, disk.bamTrack, part.sector ), CBM_SECTOR_SIZE );
    sector[2] = disk.dosVersion;
    if ( !disk.bamSharesHeader )
    {
      sector[4] = static_cast<std::uint8_t>( part.firstTrack );
      sector[5] = static_cast<std::uint8_t>( part.lastTrack + 1 );
    }
  }
  for ( std::uint32_t track = 1; track <= disk.tracks; ++track )
  {
    std::span<std::uint8_t> const entry = bamEntryFor( image, disk, track );
    std::uint32_t const sectors = cbmSectorsOn( disk, track );
    entry[0] = static_cast<std::uint8_t>( sectors );
    for ( std::uint32_t sector = 0; sector < sectors; ++sector )
    {
      entry[1 + ( sector / 8 )] |= static_cast<std::uint8_t>( 1U << ( sector % 8 ) );
    }
  }

  // The header, and the chain that runs from it through the map to the first
  // directory sector. A 1541's header is the map sector itself, so the chain
  // is one link long.
  std::span<std::uint8_t> const header =
      image.subspan( cbmOffsetOfSector( disk, disk.directoryTrack, disk.headerSector ), CBM_SECTOR_SIZE );
  header[2] = disk.dosVersion;
  writeName( header, disk );
  if ( disk.bamSharesHeader )
  {
    header[0] = static_cast<std::uint8_t>( disk.directoryTrack );
    header[1] = static_cast<std::uint8_t>( disk.firstEntrySector );
  }
  else
  {
    header[0] = static_cast<std::uint8_t>( disk.bamTrack );
    header[1] = static_cast<std::uint8_t>( disk.bam.front().sector );
    for ( std::size_t index = 0; index < disk.bam.size(); ++index )
    {
      std::span<std::uint8_t> const sector =
          image.subspan( cbmOffsetOfSector( disk, disk.bamTrack, disk.bam[index].sector ), CBM_SECTOR_SIZE );
      bool const last = index + 1 == disk.bam.size();
      sector[0] = static_cast<std::uint8_t>( last ? disk.directoryTrack : disk.bamTrack );
      sector[1] = static_cast<std::uint8_t>( last ? disk.firstEntrySector : disk.bam[index + 1].sector );
    }
  }

  // The tracks the disk keeps for itself.
  for ( std::uint32_t track : { disk.directoryTrack, disk.bamTrack } )
  {
    for ( std::uint32_t sector = 0; sector < cbmSectorsOn( disk, track ); ++sector )
    {
      markUsed( image, disk, track, sector );
    }
  }

  // The directory: one entry, and the block after it closes the chain.
  std::span<std::uint8_t> const directory =
      image.subspan( cbmOffsetOfSector( disk, disk.directoryTrack, disk.firstEntrySector ), CBM_SECTOR_SIZE );
  directory[0] = 0;
  directory[1] = 0xFF;

  // Where the Phases wait. Nothing crosses a unit, and a unit is a track, so
  // an image lies in one track's sectors and the sector it begins at is the
  // offset within that unit over what a sector holds.
  for ( Stored const& one : storedImages( build ) )
  {
    std::uint32_t const position = target.positionOf( one.at );
    std::uint32_t const track = position / target.bankSize();
    std::uint32_t at = position % target.bankSize();
    for ( std::uint8_t const byte : one.form )
    {
      std::uint32_t const sector = at / CBM_SECTOR_SIZE;
      image[cbmOffsetOfSector( disk, track, sector ) + ( at % CBM_SECTOR_SIZE )] = byte;
      markUsed( image, disk, track, sector );
      ++at;
    }
  }

  // The program's own sectors, from whatever the Phases left: a file of a CBM
  // disk is a chain, each sector giving the track and the sector of the one
  // after it and holding 254 bytes of its own, and the last giving a zero
  // track and the index of its last byte.
  std::vector<Sector> chain;
  auto const blocks = static_cast<std::uint32_t>( ( program.bytes.size() + CBM_FILE_BYTES - 1 ) / CBM_FILE_BYTES );
  for ( std::uint32_t track = 1; track <= disk.tracks && chain.size() < blocks; ++track )
  {
    for ( std::uint32_t sector = 0; sector < cbmSectorsOn( disk, track ) && chain.size() < blocks; ++sector )
    {
      if ( isFree( image, disk, track, sector ) )
      {
        chain.push_back( Sector{ .track = track, .sector = sector } );
      }
    }
  }
  if ( chain.size() < blocks )
  {
    sink.add( diag::diagnostic( diag::DiagnosticId::D64_PROGRAM_HAS_NO_ROOM )
                  .arg( "required", blocks )
                  .arg( "available", static_cast<std::uint32_t>( chain.size() ) ) );
    return CbmDiskFile{};
  }

  std::size_t written = 0;
  for ( std::size_t index = 0; index < chain.size(); ++index )
  {
    std::size_t const at = cbmOffsetOfSector( disk, chain[index].track, chain[index].sector );
    std::size_t const takes = std::min<std::size_t>( CBM_FILE_BYTES, program.bytes.size() - written );
    bool const last = index + 1 == chain.size();
    image[at] = last ? 0 : static_cast<std::uint8_t>( chain[index + 1].track );
    image[at + 1] =
        last ? static_cast<std::uint8_t>( takes + 1 ) : static_cast<std::uint8_t>( chain[index + 1].sector );
    std::ranges::copy( std::span{ program.bytes }.subspan( written, takes ),
                       image.begin() + static_cast<std::ptrdiff_t>( at + CBM_LINK_SIZE ) );
    markUsed( image, disk, chain[index].track, chain[index].sector );
    written += takes;
  }

  // The entry, which is what a `LOAD` finds the file by.
  std::span<std::uint8_t> const entry = directory.subspan( 0, CBM_ENTRY_SIZE );
  entry[2] = CLOSED_PRG;
  entry[3] = static_cast<std::uint8_t>( chain.front().track );
  entry[4] = static_cast<std::uint8_t>( chain.front().sector );
  std::ranges::fill( entry.subspan( 5, 16 ), PAD );
  for ( std::size_t index = 0; index < PROGRAM_NAME.size(); ++index )
  {
    entry[5 + index] = static_cast<std::uint8_t>( PROGRAM_NAME[index] );
  }
  entry[0x1E] = static_cast<std::uint8_t>( blocks & 0xFF );
  entry[0x1F] = static_cast<std::uint8_t>( blocks >> 8 );
  return file;
}

} // namespace nga::model
