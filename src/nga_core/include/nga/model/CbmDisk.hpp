#pragma once

#include "nga/model/Types.hpp"

#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

namespace nga::model
{

/// What a sector of a Commodore disk holds, on every image this tool writes.
constexpr std::uint32_t CBM_SECTOR_SIZE = 256;

/// A run of tracks that hold the same number of sectors. These surfaces are
/// **zoned**, the outer tracks being longer than the inner ones, so a track's
/// number does not say how many sectors are on it — which is the whole reason
/// storage here is not one size per unit.
struct CbmZone
{
  std::uint32_t first = 0;
  std::uint32_t last = 0;
  std::uint32_t sectors = 0;
};

/// One sector of the block availability map, and the tracks it accounts for.
/// A 1541 keeps its map in the same sector as the disk's name; an 8050 gives
/// the map sectors of its own, each saying which tracks it covers, and chains
/// them from the header to the first directory sector.
struct CbmBamSector
{
  std::uint32_t sector = 0;
  std::uint32_t firstTrack = 0;
  std::uint32_t lastTrack = 0;
};

/// The shape of one of the disks this tool writes. Three of them differ in
/// nothing a driver can see — a unit is a track on all three — and in four
/// things a writer must know: how the surface is zoned, where the directory
/// is, how the map is laid out, and how long the image comes to.
struct CbmGeometry
{
  std::string_view name;
  std::span<CbmZone const> zones;
  std::uint32_t tracks = 0;
  std::uint32_t sectors = 0;

  /// Where the directory is. The **header** sector carries the disk's name and
  /// links to the first sector of the map; the entries follow it.
  std::uint32_t directoryTrack = 0;
  std::uint32_t headerSector = 0;
  std::uint32_t firstEntrySector = 0;

  /// Where the map is, and how it is laid out. On a 1541 it shares the header
  /// sector and `bamTrack` is the directory's own; an 8050 puts it on the
  /// track before, in sectors of its own.
  std::uint32_t bamTrack = 0;
  std::span<CbmBamSector const> bam;
  bool bamSharesHeader = false;

  /// How many bytes of bitmap a track takes, which follows the widest zone:
  /// three on a 1541 and four on an 8050. The count of free sectors stands in
  /// front of them, so an entry is one more than this.
  std::uint32_t bitmapBytes = 0;

  /// What the DOS answers with: the version byte the header and every map
  /// sector carry, and the two characters of the type.
  std::uint8_t dosVersion = 0;
  std::string_view dosType;

  /// Whether a whole track is reserved for the directory. Both are, and an
  /// 8050's map track is reserved beside it.
  [[nodiscard]] std::uint32_t imageSize() const
  {
    return sectors * CBM_SECTOR_SIZE;
  }
};

/// The three this tool writes: a 1541's, an 8050's and an 8250's.
CbmGeometry const& cbmD64();
CbmGeometry const& cbmD80();
CbmGeometry const& cbmD82();

/// The geometry a Container names, and nothing for a Container that is not one
/// of the three. One place answers this, so that adding a fourth surface is a
/// table and not a search.
CbmGeometry const* cbmGeometryOf( Container container );

/// How many sectors track `track` holds, and zero for a number no track has.
std::uint32_t cbmSectorsOn( CbmGeometry const& disk, std::uint32_t track );

/// The widest track, which is what a Target of unequal units takes as its
/// stride: every unit is that many bytes of **position**, and the sectors a
/// narrower track does not have are held against every image. See
/// docs/spec/cbm-disk.md.
std::uint32_t cbmWidestTrack( CbmGeometry const& disk );

/// How much of each unit a program may use, indexed by the unit's number,
/// which **is** the track's: unit zero is no track, and the directory's track
/// and an 8050's map track are the disk's own, so all of those are zero and
/// nothing is ever placed in them.
std::vector<std::uint32_t> cbmUsableUnits( CbmGeometry const& disk );

/// Where sector `sector` of track `track` begins in the image, which is the
/// sectors of every track before it end to end. Tracks are numbered from one
/// and sectors from zero, as every tool that reads these images has them.
std::uint32_t cbmOffsetOfSector( CbmGeometry const& disk, std::uint32_t track, std::uint32_t sector );

/// What a sector of a **file** carries: the first two bytes are the track and
/// sector of the one after it, so a file's own bytes are the rest.
constexpr std::uint32_t CBM_LINK_SIZE = 2;
constexpr std::uint32_t CBM_FILE_BYTES = CBM_SECTOR_SIZE - CBM_LINK_SIZE;

/// A directory holds eight entries to the sector, each of this many bytes.
constexpr std::uint32_t CBM_ENTRY_SIZE = 32;

} // namespace nga::model
