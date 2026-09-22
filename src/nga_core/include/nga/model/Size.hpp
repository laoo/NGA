#pragma once

#include "nga/diag/DiagnosticSink.hpp"
#include "nga/model/Build.hpp"
#include "nga/model/Isa.hpp"
#include "nga/model/Merge.hpp"
#include "nga/model/Types.hpp"

#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace nga::model
{

/// The part of a Section that holds initialised bytes, as offsets from its
/// start: from the first Chunk that emits to the end of the last. Reserved
/// space between those lies inside it and travels with the Section; reserved
/// space at either end lies outside, is in no Container and is written by no
/// load. Empty for a Section of reservations alone. See
/// docs/decisions/0026-a-payload-is-the-initialised-extent.md.
struct InitialisedExtent
{
  std::uint32_t begin = 0;
  std::uint32_t end = 0;

  [[nodiscard]] std::uint32_t size() const
  {
    return end - begin;
  }

  friend bool operator==( InitialisedExtent, InitialisedExtent ) = default;
};

/// The sizes inside a macro use's expansion: how far each inner Chunk stands
/// from the use's own offset, one entry longer than there are inner Chunks,
/// and the addressing mode Size settled each inner instruction to. Empty for
/// every Chunk that is not a use.
struct InnerSizes
{
  std::vector<std::uint32_t> offsets;
  std::vector<std::optional<AddressingMode>> modes;
};

/// The result of the Size Step: how large every Chunk is, and how far it
/// stands from the start of its Section.
///
/// Held apart from the Chunks, which carry only what Assemble knew — see
/// docs/decisions/0012-step-results-are-separate-objects.md. Nothing in here
/// was decided by looking at an address, because when this is built there are
/// none.
class Sizes
{
public:
  explicit Sizes( std::span<Module const> modules );

  /// Records one Section, once. Offsets and the initialised extent are derived
  /// here rather than supplied, which is what keeps them from ever disagreeing
  /// with the sizes; `emits` says of each Chunk whether it emits bytes.
  void store( SectionRef where,
              std::vector<std::uint32_t> sizes,
              std::vector<std::optional<AddressingMode>> modes,
              std::vector<bool> const& emits,
              std::vector<InnerSizes> inner = {} );

  [[nodiscard]] bool isKnown( SectionRef where ) const;

  [[nodiscard]] std::uint32_t sizeOfChunk( SectionRef where, ChunkIndex chunk ) const;

  /// Legal for an index equal to the Chunk count, which is the position after
  /// the last Chunk and what lets a Label stand at the end of a Section.
  [[nodiscard]] std::uint32_t offsetOf( SectionRef where, ChunkIndex chunk ) const;

  [[nodiscard]] std::uint32_t sizeOfSection( SectionRef where ) const;

  /// What of the Section is initialised. Its size is what a Container carries
  /// and a Transition loads; `sizeOfSection` is what the Section occupies.
  [[nodiscard]] InitialisedExtent initialisedExtentOf( SectionRef where ) const;

  /// What Size settled an instruction's addressing mode to be, so that Patch
  /// writes the bytes of the width that was decided here rather than deciding
  /// it a second time.
  [[nodiscard]] std::optional<AddressingMode> modeOf( SectionRef where, ChunkIndex chunk ) const;

  /// The inside of a macro use, by inner index: where an inner Chunk stands
  /// from the start of the Section, how large it is, and the mode Size
  /// settled it to. The use's own size is the sum, and its own mode nothing.
  [[nodiscard]] std::uint32_t innerOffsetOf( SectionRef where, ChunkIndex chunk, std::uint32_t inner ) const;
  [[nodiscard]] std::uint32_t innerSizeOf( SectionRef where, ChunkIndex chunk, std::uint32_t inner ) const;
  [[nodiscard]] std::optional<AddressingMode>
  innerModeOf( SectionRef where, ChunkIndex chunk, std::uint32_t inner ) const;

private:
  struct OneSection
  {
    bool known = false;
    std::vector<std::uint32_t> sizes;
    std::vector<std::uint32_t> offsets; ///< one longer than sizes
    std::vector<std::optional<AddressingMode>> modes;
    std::vector<InnerSizes> inner;
    InitialisedExtent initialised;
  };

  [[nodiscard]] OneSection const& at( SectionRef where ) const;

  std::vector<std::vector<OneSection>> mByModule;
};

/// The Size Step. Resolves every Chunk's size from the PlacementClass of what
/// its operand names, from what its data counts, or from an Integer expression
/// given to `.res`; and a Jcc's from the distance to what it names, along the
/// `then` chain it stands in.
///
/// A reservation may depend on other sizes and a cycle among them is an error;
/// nothing here may depend on an address, and nothing can, because Place has
/// not run.
Sizes computeSizes( Merged const& build, diag::DiagnosticSink& sink );

} // namespace nga::model
