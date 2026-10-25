#pragma once

#include "nga/model/Indices.hpp"

#include <cstdint>
#include <string>
#include <string_view>

namespace nga::model
{

// The cartridge boards a Project may name, as the Project holds them once its
// document has been read: resolved there, against the boards the tool knows,
// so that every writer and every check reads the one answer. What the names
// are and how one is looked up is each Container's own — see Car.hpp and
// Lnx.hpp.

/// One cartridge board the tool writes an image for — see docs/spec/car.md and
/// docs/decisions/0216-a-car-names-its-format-and-the-cold-start-is-an-edge.md.
///
/// Eight, and not the hundred-odd mappers that exist, because a program of this
/// tool has code that must be reachable *while* a bank is switched and so needs
/// a part of the ROM no switch takes away. Only the XEGS family gives that in
/// the shape the tool wants; the two without banks give the whole of it.
enum class CartridgeFormat : std::uint8_t
{
  STANDARD_8K,
  STANDARD_16K,
  XEGS_32K,
  XEGS_64K,
  XEGS_128K,
  XEGS_256K,
  XEGS_512K,
  XEGS_1024K,
};

/// What a board is, as the writer and the check against the Target read it.
struct Cartridge
{
  /// As the Project writes it, quoted: `"8k"`, `"xegs128"`.
  std::string_view name;

  CartridgeFormat format = CartridgeFormat::STANDARD_8K;

  /// The number the sixteen-byte header carries, which is what a reader knows
  /// the board by.
  std::uint32_t mapper = 0;

  /// The image's size, exactly: a reader resizes what it is given to this, so
  /// writing anything else is writing a file whose meaning depends on who
  /// opens it.
  std::uint32_t imageSize = 0;

  /// The part of the address space no switch takes away, and where it begins
  /// in the image — the **last** bank of it on a XEGS board.
  AddressRange fixed{};
  std::uint32_t fixedOffset = 0;

  /// The switched part, and how many Banks of it are storage: one fewer than
  /// the board has, since the last is the fixed part under another name. Both
  /// empty on a board with no banking, which is what `units == 0` says.
  AddressRange window{};
  std::uint32_t units = 0;
  std::uint32_t bankSize = 0;

  [[nodiscard]] bool isBanked() const
  {
    return units > 0;
  }
};

/// One of the four geometries a board is wired for, named as a Project writes
/// it: the page size in bytes, and the image's whole size, which is 256 pages of
/// it.
struct LynxBoard
{
  std::string_view name;
  std::uint32_t pageSize;
};

/// What a `cartridge` block said, for the header a Lynx cartridge carries —
/// see docs/spec/lnx.md. Empty strings where nothing said, which is what the
/// header then holds: a name invented from the file's own would be a name the
/// reader shows and nobody chose.
struct LynxHeader
{
  std::string name{};
  std::string maker{};
  std::uint8_t rotation = 0;
};

} // namespace nga::model
