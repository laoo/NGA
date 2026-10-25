#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace nga::model
{

/// The ZX0 stored form of some bytes: Einar Saukas's format, the standard
/// forward stream of version 2, which the `zx0` transform's 6502 half turns
/// back. Optimal parsing, so it is slow in proportion to the input times the
/// window — a second for a Bank-sized Section — and it is deterministic. The
/// host half of a pair the suite holds to being inverses by running both;
/// see docs/decisions/0027-a-transform-declares-its-zero-page-and-zx0.md.
///
/// A port of the reference compressor, under its BSD licence — the notice is
/// in Zx0.cpp. Ported rather than carried, because the reference reports its
/// progress on standard output and exits the process on failure, neither of
/// which a library may do, and a carried file is not edited.
[[nodiscard]] std::vector<std::uint8_t> encodeZx0( std::span<std::uint8_t const> from );

/// How far past the end of what it decodes to a stored form has to end for
/// the decoder to turn it back **in place** — the stream loaded against the
/// top of the destination and read forward while the output overtakes it —
/// without writing over a byte it has not read yet: ZX0's own `delta`. Found
/// by reading the form as the 6502 half does, the two size bytes first and a
/// byte whenever the bit buffer runs dry, and counting what has been written
/// before every byte is read. Nothing where the form is not one this format
/// reads to its end — see
/// docs/decisions/0235-a-xex-decodes-in-place-while-it-loads.md.
[[nodiscard]] std::optional<std::uint32_t> zx0InPlaceMargin( std::span<std::uint8_t const> form );

} // namespace nga::model
