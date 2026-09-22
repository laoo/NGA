#pragma once

#include <cstdint>
#include <string_view>

namespace nga::model
{

/// Whether an address lives in the zero page or anywhere else.
///
/// Declared, never inferred: instruction size depends on it and addresses
/// depend on instruction sizes, so reading it back from the address the solver
/// chose would close that loop. A Section declares it and its Labels inherit it.
enum class PlacementClass : std::uint8_t
{
  ZEROPAGE,
  ABSOLUTE,
};

std::string_view nameOf( PlacementClass placement );

} // namespace nga::model
