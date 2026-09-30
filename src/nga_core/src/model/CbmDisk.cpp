#include "nga/model/CbmDisk.hpp"

#include "nga/model/Project.hpp"

#include <algorithm>
#include <array>

namespace nga::model
{

namespace
{

/// A 1541's and a 4040's surface: twenty-one sectors on the outermost
/// seventeen tracks and seventeen on the innermost five, 683 in all.
constexpr std::array<CbmZone, 4> D64_ZONES{ { { .first = 1, .last = 17, .sectors = 21 },
                                              { .first = 18, .last = 24, .sectors = 19 },
                                              { .first = 25, .last = 30, .sectors = 18 },
                                              { .first = 31, .last = 35, .sectors = 17 } } };

/// An 8050's, which is one side of an 8250's: twenty-nine down to twenty-three
/// over seventy-seven tracks, 2083 in all.
constexpr std::array<CbmZone, 4> D80_ZONES{ { { .first = 1, .last = 39, .sectors = 29 },
                                              { .first = 40, .last = 53, .sectors = 27 },
                                              { .first = 54, .last = 64, .sectors = 25 },
                                              { .first = 65, .last = 77, .sectors = 23 } } };

/// An 8250's: the same four zones again for the second side, 4166 in all.
constexpr std::array<CbmZone, 8> D82_ZONES{ { { .first = 1, .last = 39, .sectors = 29 },
                                              { .first = 40, .last = 53, .sectors = 27 },
                                              { .first = 54, .last = 64, .sectors = 25 },
                                              { .first = 65, .last = 77, .sectors = 23 },
                                              { .first = 78, .last = 116, .sectors = 29 },
                                              { .first = 117, .last = 130, .sectors = 27 },
                                              { .first = 131, .last = 141, .sectors = 25 },
                                              { .first = 142, .last = 154, .sectors = 23 } } };

/// A 1541 keeps the map in the header sector itself, so there is one entry and
/// it names the sector both of them are in.
constexpr std::array<CbmBamSector, 1> D64_BAM{ { { .sector = 0, .firstTrack = 1, .lastTrack = 35 } } };

/// An 8050 gives the map sectors of its own on the track before the
/// directory's, each covering fifty tracks: fifty entries of five bytes and
/// six of header come to exactly a sector.
constexpr std::array<CbmBamSector, 2> D80_BAM{ { { .sector = 0, .firstTrack = 1, .lastTrack = 50 },
                                                 { .sector = 3, .firstTrack = 51, .lastTrack = 77 } } };

constexpr std::array<CbmBamSector, 4> D82_BAM{ { { .sector = 0, .firstTrack = 1, .lastTrack = 50 },
                                                 { .sector = 3, .firstTrack = 51, .lastTrack = 100 },
                                                 { .sector = 6, .firstTrack = 101, .lastTrack = 150 },
                                                 { .sector = 9, .firstTrack = 151, .lastTrack = 154 } } };

} // namespace

CbmGeometry const& cbmD64()
{
  static CbmGeometry const DISK{ .name = "d64",
                                 .zones = D64_ZONES,
                                 .tracks = 35,
                                 .sectors = 683,
                                 .directoryTrack = 18,
                                 .headerSector = 0,
                                 .firstEntrySector = 1,
                                 .bamTrack = 18,
                                 .bam = D64_BAM,
                                 .bamSharesHeader = true,
                                 .bitmapBytes = 3,
                                 .dosVersion = 'A',
                                 .dosType = "2A" };
  return DISK;
}

CbmGeometry const& cbmD80()
{
  static CbmGeometry const DISK{ .name = "d80",
                                 .zones = D80_ZONES,
                                 .tracks = 77,
                                 .sectors = 2083,
                                 .directoryTrack = 39,
                                 .headerSector = 0,
                                 .firstEntrySector = 1,
                                 .bamTrack = 38,
                                 .bam = D80_BAM,
                                 .bamSharesHeader = false,
                                 .bitmapBytes = 4,
                                 .dosVersion = 'C',
                                 .dosType = "2C" };
  return DISK;
}

CbmGeometry const& cbmD82()
{
  static CbmGeometry const DISK{ .name = "d82",
                                 .zones = D82_ZONES,
                                 .tracks = 154,
                                 .sectors = 4166,
                                 .directoryTrack = 39,
                                 .headerSector = 0,
                                 .firstEntrySector = 1,
                                 .bamTrack = 38,
                                 .bam = D82_BAM,
                                 .bamSharesHeader = false,
                                 .bitmapBytes = 4,
                                 .dosVersion = 'C',
                                 .dosType = "2C" };
  return DISK;
}

CbmGeometry const* cbmGeometryOf( Container container )
{
  switch ( container )
  {
  case Container::D64:
    return &cbmD64();
  case Container::D80:
    return &cbmD80();
  case Container::D82:
    return &cbmD82();
  default:
    return nullptr;
  }
}

std::uint32_t cbmSectorsOn( CbmGeometry const& disk, std::uint32_t track )
{
  auto const found = std::ranges::find_if(
      disk.zones, [track]( CbmZone const& zone ) { return track >= zone.first && track <= zone.last; } );
  return found == disk.zones.end() ? 0 : found->sectors;
}

std::uint32_t cbmWidestTrack( CbmGeometry const& disk )
{
  std::uint32_t widest = 0;
  for ( CbmZone const& zone : disk.zones )
  {
    widest = std::max( widest, zone.sectors );
  }
  return widest * CBM_SECTOR_SIZE;
}

std::vector<std::uint32_t> cbmUsableUnits( CbmGeometry const& disk )
{
  std::vector<std::uint32_t> usable( disk.tracks + 1, 0 );
  for ( std::uint32_t track = 1; track <= disk.tracks; ++track )
  {
    // The directory's track is the disk's, and so is the map's where it has one
    // of its own: an 8050 keeps it on the track before the directory, and the
    // sectors of that track the map does not take are not worth the rule it
    // would cost a program to reach them.
    if ( track == disk.directoryTrack || track == disk.bamTrack )
    {
      continue;
    }
    usable[track] = cbmSectorsOn( disk, track ) * CBM_SECTOR_SIZE;
  }
  return usable;
}

std::uint32_t cbmOffsetOfSector( CbmGeometry const& disk, std::uint32_t track, std::uint32_t sector )
{
  std::uint32_t before = 0;
  for ( std::uint32_t earlier = 1; earlier < track; ++earlier )
  {
    before += cbmSectorsOn( disk, earlier );
  }
  return ( before + sector ) * CBM_SECTOR_SIZE;
}

} // namespace nga::model
