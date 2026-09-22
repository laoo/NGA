#pragma once

#include "nga/model/Project.hpp"

#include <cstddef>
#include <cstdint>

namespace nga::model
{

/// How a Dispatch is written, which the Target's processor and the number of
/// targets decide between — see
/// docs/decisions/0191-a-dispatch-goes-to-one-of-its-own-positions.md.
///
/// `INDEXED` is `jmp (abs,x)` over one table of addresses, which indexes
/// pairs and so reaches 128 of them. `RETURNED` is the `rts` trick over two
/// half tables holding each target less one, which indexes bytes and reaches
/// 256; every processor has it, and it is what a 6502 has instead.
enum class DispatchForm : std::uint8_t
{
  INDEXED,
  RETURNED,
};

/// The most targets a Dispatch may name, which is what the `rts` trick's
/// index reaches.
constexpr std::size_t MOST_DISPATCH_TARGETS = 256;

/// The most an `INDEXED` Dispatch may name: `jmp (abs,x)` indexes pairs, so
/// the doubled value must stay inside a byte.
constexpr std::size_t MOST_INDEXED_TARGETS = 128;

/// The shortest form that holds this many targets on this processor.
[[nodiscard]] DispatchForm dispatchFormOf( Cpu cpu, std::size_t targets );

/// What a Dispatch of this form and this many targets occupies: the
/// instructions that jump, and then two bytes a target either way.
[[nodiscard]] std::uint32_t sizeOfDispatch( DispatchForm form, std::size_t targets );

/// Where the table of an `INDEXED` Dispatch begins, and where the high and
/// low halves of a `RETURNED` one do, as offsets from the Chunk's own
/// address.
[[nodiscard]] std::uint32_t tableOffsetOf( DispatchForm form );

} // namespace nga::model
