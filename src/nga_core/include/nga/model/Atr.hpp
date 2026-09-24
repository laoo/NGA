#pragma once

#include "nga/diag/SourceManager.hpp"
#include "nga/model/Project.hpp"

#include <cstdint>
#include <string>
#include <string_view>

namespace nga::model
{

/// A double-density diskette, which is what the `.atr` this tool writes
/// describes: **256-byte sectors**. The boot record is the exception the
/// hardware makes — the OS boots with `DSCTLN` at 128, so the first three
/// sectors hold 128 bytes each and the file has a step at the fourth. That is
/// the shape every reader expects of a double-density image; see
/// docs/spec/atr.md.
constexpr std::uint32_t ATR_SECTOR_SIZE = 256;
constexpr std::uint32_t ATR_BOOT_SECTOR_SIZE = 128;

/// A diskette is 720 sectors, 180 KB, and an image holds at least that many so
/// that an ordinary program comes out as a diskette rather than as something
/// only a hard disk would mount. A program needing more grows the image, up to
/// what a sector number reaches: `DAUX1` and `DAUX2` are two bytes.
constexpr std::uint32_t ATR_DISKETTE_SECTORS = 720;
constexpr std::uint32_t ATR_MOST_SECTORS = 65535;

/// The boot record: the sectors the OS reads before anything of the tool's
/// own runs, where it puts them, and where it calls once they stand there.
/// Three because that is what a diskette's boot record has always been, and
/// the loader fits in them with room to spare.
constexpr std::uint32_t ATR_BOOT_SECTORS = 3;
constexpr std::uint32_t ATR_BOOT_ADDRESS = 0x0700;
constexpr std::uint32_t ATR_BOOT_SIZE = ATR_BOOT_SECTORS * ATR_BOOT_SECTOR_SIZE;

/// The sector unit zero of storage stands in. Sectors are numbered from one,
/// so this is the first after the boot record — a constant the Container and
/// the driver both hold, since neither can tell the other.
constexpr std::uint32_t ATR_FIRST_STORAGE_SECTOR = ATR_BOOT_SECTORS + 1;

/// What a unit of storage holds, and the only size the Container accepts: 256
/// sectors of 256 bytes. It makes the three bytes the model addresses storage
/// with — a unit and a two-byte offset — the address the hardware takes: the
/// unit is the high byte of the sector number, the offset's high byte is the
/// low one, and the offset's low byte is the position in the sector. The
/// driver then does no arithmetic at all.
constexpr std::uint32_t ATR_UNIT_SIZE = ATR_SECTOR_SIZE * 256;

/// Where a sector begins in the file, past the header: the first three hold
/// 128 bytes and the rest 256.
[[nodiscard]] constexpr std::uint32_t atrOffsetOfSector( std::uint32_t sector )
{
  std::uint32_t const before = sector - 1;
  return before <= ATR_BOOT_SECTORS ? before * ATR_BOOT_SECTOR_SIZE
                                    : ATR_BOOT_SIZE + ( ( before - ATR_BOOT_SECTORS ) * ATR_SECTOR_SIZE );
}

/// The Module name of the boot record: dotted, as the tool's own Modules are,
/// so that no file of a Project is named so.
constexpr std::string_view BOOT_MODULE = "nga.boot";

/// Where in the boot record the Container writes the first sector of the load
/// image: a Label of the Module below, exported, found by name as the `.xex`
/// finds the cell it writes a unit into. It is patched rather than written
/// into the text because it follows the storage the program came to **use**,
/// which nothing knows until every Payload has been placed.
constexpr std::string_view BOOT_IMAGE_NAME = "ngaBootImage";

/// The text of the boot record: the six bytes the OS reads a boot from, and
/// the loader it calls.
std::string bootRecordSource();

/// Adds the boot record to a Project whose Container is the `.atr`, as a
/// Module every Phase needs: its loader runs before the program does, and its
/// `DOSINI` is called on every reset for as long as the program lives.
void addBootRecord( Project& project, diag::SourceManager& sources );

} // namespace nga::model
