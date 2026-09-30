#pragma once

#include "nga/model/Build.hpp"

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace nga::model
{

/// A cartridge of this machine holds 256 pages per strobe, and the page is as
/// wide as the board is wired for: the shift register is always eight bits, so
/// the geometry is one number. What a reader with no header divides the file's
/// length by is that same 256 — see docs/spec/lnx.md.
constexpr std::uint32_t LYNX_PAGES = 256;

/// The header a `.lnx` carries and a `.lyx` does not, and the two fields of it a
/// reader shows: a name and a manufacturer, each ending in a byte a reader writes
/// a terminator over, so what fits is one less than the field is wide.
constexpr std::uint32_t LNX_HEADER_SIZE = 64;
constexpr std::uint32_t LNX_NAME_SIZE = 32;
constexpr std::uint32_t LNX_MAKER_SIZE = 16;

/// `none`, `left` or `right` as the header numbers them, and nothing where the
/// word names no rotation.
[[nodiscard]] std::optional<std::uint8_t> lynxRotationNamed( std::string_view word );

/// One encrypted block: fifty plaintext bytes, and the fifty-first is the `$15`
/// the boot ROM checks. The ROM reads exactly this many for every block, which
/// is why the number is a length and not a maximum.
constexpr std::uint32_t LYNX_BLOCK = 50;
constexpr std::uint32_t LYNX_CHUNK = LYNX_BLOCK + 1;

/// The most blocks the boot ROM will read, which it decides by refusing a count
/// byte below `$FB`. Five, so 250 bytes of loader — and the loader this tool
/// writes is one block, which is why the rest of it is read unencrypted.
constexpr std::uint32_t LYNX_MOST_BLOCKS = 5;

/// One of the four geometries a board is wired for, named as a Project writes
/// it: the page size in bytes, and the image's whole size, which is 256 pages of
/// it.
struct LynxBoard
{
  std::string_view name;
  std::uint32_t pageSize;
};

/// In the order a finding lists them.
std::span<LynxBoard const> lynxBoards();

/// The board of that name, and nothing where no board has it.
[[nodiscard]] std::optional<LynxBoard> lynxBoardNamed( std::string_view name );

/// The loader as the boot ROM reads it: a count byte and then a 51-byte block
/// per fifty bytes of `plain`, each the delta of the bytes before it raised to
/// the private exponent. Empty where `plain` is longer than the ROM will read or
/// does not end where the accumulator has to.
///
/// Every block is **exactly** 51 bytes, which is the thing to get wrong: a
/// number written without its leading zeroes — as about one block in fifty would
/// be — shifts every block after it, and the ROM reads a fixed 51 for each.
[[nodiscard]] std::vector<std::uint8_t> encryptLoader( std::span<std::uint8_t const> plain );

/// The other side of it, which is what the machine's own ROM does: every 51-byte
/// block raised to the **public** exponent 3, the highest byte of each held to
/// `$15`, and the differences summed back into bytes. Empty where a block fails
/// either check, as the ROM stops there.
///
/// Here because the suite needs it and Atari's ROM is not ours to carry: an
/// independent statement about what the Container wrote is what a golden is for,
/// and this is the same arithmetic under a different exponent, which is why the
/// encryption is also held to bytes written elsewhere — see
/// docs/decisions/0106-rival-numbers-are-measured-here.md.
[[nodiscard]] std::vector<std::uint8_t> decryptLoader( std::span<std::uint8_t const> blocks );

/// The Module name of the bootstrap: dotted, as the tool's own Modules are, so
/// that no file of a Project is named so.
constexpr std::string_view LYNX_LOADER_MODULE = "nga.lynx";

/// Its two exported Labels, whose operands the Container fills: the page the
/// load image begins at, and the address the program starts at.
constexpr std::string_view LYNX_PAGE_NAME = "ngaLynxPage";
constexpr std::string_view LYNX_RUN_NAME = "ngaLynxRun";

/// The bootstrap's text, for the board's page size — the loader counts a page in
/// 256-byte chunks, which is the one number of the geometry it needs.
[[nodiscard]] std::string lynxLoaderSource( std::uint32_t pageSize );

/// Added to a Project whose Container is a Lynx cartridge, as the `.atr`'s boot
/// record is added to one that boots a diskette.
void addLynxLoader( Project& project, diag::SourceManager& sources );

} // namespace nga::model
