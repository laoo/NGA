#pragma once

#include "nga/diag/DiagnosticSink.hpp"
#include "nga/model/Build.hpp"
#include "nga/model/Merge.hpp"
#include "nga/model/Place.hpp"
#include "nga/model/Project.hpp"
#include "nga/model/Size.hpp"
#include "nga/model/Storage.hpp"

#include <cstdint>
#include <span>
#include <vector>

namespace nga::model
{

/// The result of the Patch Step: the bytes of every Section.
///
/// One buffer per Section rather than one per Chunk. A Chunk is a description
/// of what to emit and never a byte buffer — see
/// docs/decisions/0012-step-results-are-separate-objects.md — so the bytes
/// come into existence here, all at once, at the size Size settled on.
class Bytes
{
public:
  explicit Bytes( std::span<Module const> modules );

  void store( SectionRef where, std::vector<std::uint8_t> bytes );

  [[nodiscard]] std::span<std::uint8_t const> of( SectionRef where ) const;

  /// What one Chunk emitted, which is what a `~bytes` expectation compares.
  [[nodiscard]] std::span<std::uint8_t const> ofChunk( SectionRef where, ChunkIndex chunk, Sizes const& sizes ) const;

private:
  std::vector<std::vector<std::vector<std::uint8_t>>> mByModule;
};

/// The Patch Step: writes every resolved value into the bytes of its Chunk.
///
/// Everything it needs has been decided elsewhere — the width by Size, the
/// address by Place — so nothing here chooses anything. It writes.
///
/// A `.transition` names where its Frames wait, which the first half of
/// PlaceStorage settled before this runs; nothing else here reads storage.
/// See docs/decisions/0037-the-statement-names-its-frames.md.
Bytes patch( Placed const& build, Storage const& storage, diag::DiagnosticSink& sink );

/// The Frames, into Storage where the first half of PlaceStorage put them.
///
/// After the second half, because a Frame names where each Payload waits,
/// and PlaceStorage packs the Banks by what the stored forms came to, which
/// are made from the bytes `patch` produced. A Frame is never transformed and
/// its size was known all along, so this is only writing. See
/// docs/decisions/0036-frames.md.
void patchFrames( Placed const& build, Storage& storage, diag::DiagnosticSink& sink );

} // namespace nga::model
