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

/// The `CART` header in front of the board's image in a `.car`: the word, the
/// board's number and the image's checksum, and four bytes of nothing.
constexpr std::uint32_t CAR_HEADER_SIZE = 16;

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
