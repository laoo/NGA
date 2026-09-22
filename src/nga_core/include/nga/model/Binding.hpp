#pragma once

#include <cstdint>
#include <string_view>

namespace nga::model
{

/// How a Reference reaches a Slot's live definition: through the Cell the
/// Transition routine rewrites on every edge. See the glossary. `bank` waits
/// for Views, and `address` is not a Cell at all — it is what a Section that
/// never moves has, and a Slot is for what does.
enum class Binding : std::uint8_t
{
  /// Two bytes holding the definition's address: `lda (slot),y`, `jmp (slot)`.
  POINTER,

  /// `jmp` and two bytes: `jsr slot` calls whatever is live.
  VECTOR,
};

std::string_view nameOf( Binding binding );

/// What a Cell of each Binding occupies.
constexpr std::uint32_t POINTER_CELL_SIZE = 2;
constexpr std::uint32_t VECTOR_CELL_SIZE = 3;

} // namespace nga::model
