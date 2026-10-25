#pragma once

#include "nga/syntax/Statement.hpp"

#include <cstdint>

namespace nga::model
{

/// What a Cell of each Binding occupies.
constexpr std::uint32_t POINTER_CELL_SIZE = 2;
constexpr std::uint32_t VECTOR_CELL_SIZE = 3;

} // namespace nga::model
