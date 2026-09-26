#pragma once

#include "nga/diag/SourceManager.hpp"
#include "nga/model/Project.hpp"

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace nga::model
{

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

/// The Module name of the cartridge header, dotted as the tool's own Modules
/// are, so that no file of a Project is named so.
constexpr std::string_view CART_MODULE = "nga.cart";

/// Where the OS takes the program's start from, which the Container patches
/// once addresses exist: a Label of that Module, exported, found by name as
/// the `.atr` finds the sector it writes into its boot record.
constexpr std::string_view CART_START_NAME = "ngaCartStart";

/// The six bytes at `$BFFA` the OS reads a cartridge from, and the `rts` its
/// init vector points at.
[[nodiscard]] std::string cartHeaderSource();

/// Adds them to a Project whose Container is a `.car`, as a Module every Phase
/// needs: the header is in the address space for the whole run, and the OS
/// jumps through it again on every reset.
void addCartHeader( Project& project, diag::SourceManager& sources );

/// Every board, in the order a finding lists them.
[[nodiscard]] std::span<Cartridge const> cartridges();

/// The board that name spells, or nothing.
[[nodiscard]] std::optional<Cartridge> cartridgeNamed( std::string_view name );

/// The boards, as a finding lists them: `` `8k`, `16k`, … ``.
[[nodiscard]] std::string knownCartridges();

/// What the Target must say for the board the Project named, checked when the
/// document has been read and reported against both. A `rom` Region over the
/// fixed part, and for a banked board a Window of one range over the switched
/// part with a unit set of the right count behind it: the geometry lives in the
/// Variant, where every truth about addresses lives, and the two are held to
/// each other rather than either being derived from the other — see
/// docs/decisions/0216-a-car-names-its-format-and-the-cold-start-is-an-edge.md.
void checkCartridgeAgainstTarget( Cartridge const& board,
                                  Target const& target,
                                  diag::SourceSpan where,
                                  diag::DiagnosticSink& sink );

} // namespace nga::model
