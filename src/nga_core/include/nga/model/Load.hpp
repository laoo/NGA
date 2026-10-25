#pragma once

#include "nga/diag/DiagnosticSink.hpp"
#include "nga/model/Build.hpp"
#include "nga/model/Patch.hpp"
#include "nga/model/Place.hpp"
#include "nga/model/Prune.hpp"
#include "nga/model/Storage.hpp"

#include <cstddef>
#include <span>
#include <string>
#include <vector>

namespace nga::model
{

/// The Transform Step's half for a `.xex` without storage: every Section named
/// in a `transform` is encoded, and its margin found — how far past the end of
/// its initialised extent the form has to end for its decoder to turn it back
/// in place while the program loads. One that encoding does not shorten, or
/// whose margin runs into memory the loader may not write, is written as it is,
/// with a warning that says which and why. Nothing for any other program — see
/// docs/decisions/0235-a-xex-decodes-in-place-while-it-loads.md.
void transformAtLoad( Placed const& build, Storage& storage, Bytes const& bytes, diag::DiagnosticSink& sink );

/// What a `.xex` moves under a ROM while it loads: every Section with bytes
/// the entry Phase holds whose bytes lie under a ROM the driver switches,
/// which the entry Phase has in. Found before the transforms, which leave such
/// a Section alone. Nothing where nothing can hide — see
/// docs/decisions/0241-bytes-kept-for-a-later-phase-may-lie-hidden-under-a-rom.md.
void findHiddenAtLoad( Placed const& build, Storage& storage, Bytes const& bytes, diag::DiagnosticSink& sink );

/// The batches of that move, once `nga.load` stands where it does: the hidden
/// bytes packed behind a table into the largest stretch of memory free while
/// the program loads, as many to a batch as it holds, a Section larger than
/// that split across batches.
void stageHiddenAtLoad( Placed const& build, Storage& storage, diag::DiagnosticSink& sink );

/// A decoder's source as the Project file found it: the format it decodes,
/// the file it was read from, and its text.
struct DecoderSource
{
  std::string format;
  std::string path;
  std::string text;
};

/// The text of `nga.load`: what a `.xex` decodes with while it loads — its
/// transformed Sections without storage, and its segments where it is
/// compressed — and the move that puts what lies hidden under a ROM in place:
/// for each decoder its own text, in a Namespace of its own, and the two Procs
/// the loader enters it by. A decoder whose `.transform` line names no Label is
/// left out, and the `.xex` then has no glue to call. See
/// docs/decisions/0235-a-xex-decodes-in-place-while-it-loads.md,
/// 0241-bytes-kept-for-a-later-phase-may-lie-hidden-under-a-rom.md and
/// 0259-a-xex-may-be-compressed-segment-by-segment.md.
[[nodiscard]] std::string loadModuleSource( std::span<DecoderSource const> decoders );

/// The text of `nga.choose`, for a `choose` block of `choices` documents: the
/// byte the program chooses in, zero until it does, and the routine that walks
/// the entries of the list between the two words and writes into each address
/// the byte the Choice has there. An entry is the address, low byte first, and
/// a byte for each Choice but the first — see
/// docs/decisions/0302-a-document-may-be-chosen-while-a-xex-loads.md.
[[nodiscard]] std::string chooseModuleSource( std::size_t choices );

/// What of memory above the zero page nothing the file writes while it loads
/// stands on, and nothing that runs then reaches: where `nga.choose` stands,
/// and what is left of it a list of entries may stand in — see
/// docs/decisions/0302-a-document-may-be-chosen-while-a-xex-loads.md.
[[nodiscard]] std::vector<AddressRange> keptWhileLoading( Placed const& build, Storage const& storage );

/// A stored form as a decoder at the load reads it: the two size bytes it reads
/// past, then the bytes as the encoder left them.
[[nodiscard]] std::vector<std::uint8_t> storedFormAtLoad( std::vector<std::uint8_t> const& encoded );

/// The memory a `.xex`'s loader may write while it loads, sorted and disjoint:
/// the pools and the Windows, less what the machine holds then and less the
/// stack page, which whatever the loader calls runs on.
[[nodiscard]] std::vector<AddressRange> writableAtLoad( Placed const& build );

/// What the loader of a `.xex` calls through `INITAD` once the start is
/// written — the driver's `init` and the glue that shows a unit — and every
/// Section that code reaches: what has to be in memory before the units are
/// filled — see docs/decisions/0259-a-xex-may-be-compressed-segment-by-segment.md.
[[nodiscard]] Reachable calledByTheLoader( Placed const& build );

/// The load Step: places `nga.load` — the decoders and the glue the loader
/// calls — in memory free while the program loads, and patches it. Free means
/// in a pool, clear of every Section the load decodes and the margin above it,
/// and clear of every `root` reservation the entry Phase holds, which is what
/// the OS and the hardware use while the loader runs. Everything else of the
/// program is a reservation nobody has written yet, or bytes the `.xex` writes
/// after the last decode. A compressed file decodes into the Windows and after
/// what the loader calls is written, so there it keeps off both, and prefers
/// memory no segment of the file writes. Nothing where nothing is decoded at
/// load.
void placeLoad( Placed const& build, Layout& layout, Storage const& storage, Bytes& bytes, diag::DiagnosticSink& sink );

} // namespace nga::model
