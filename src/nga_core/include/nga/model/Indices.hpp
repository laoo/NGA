#pragma once

#include <cstdint>

namespace nga::model
{

// The small values the model names things by — a position in one of its
// lists, each a type of its own so that one cannot be passed for another,
// and a range of addresses. Apart from Project.hpp, which declares the lists,
// so that a header naming one — a Chunk's, a Section's — need not read the
// whole Project to do it.

/// A Module's position in the Project.
///
/// Project order is the tie-break the ordering contract in
/// docs/spec/diagnostics.md rests on, and it is also the order files are
/// registered with the SourceManager, so the two never have to be reconciled.
struct ModuleIndex
{
  std::uint32_t value = 0;

  friend bool operator==( ModuleIndex, ModuleIndex ) = default;
};

/// A macro's position within its Module's list, which is declaration order.
/// Beside the Module's own index because the Project names one: the
/// driver's roles are macros — see Driver.
struct MacroIndex
{
  std::uint32_t value = 0;

  friend bool operator==( MacroIndex, MacroIndex ) = default;
};

/// A Phase's position in the PhaseGraph, which is declaration order.
struct PhaseIndex
{
  std::uint32_t value = 0;

  friend bool operator==( PhaseIndex, PhaseIndex ) = default;
};

/// A Region's position in the Target's list, which is declaration order.
struct RegionIndex
{
  std::uint32_t value = 0;

  friend bool operator==( RegionIndex, RegionIndex ) = default;
};

/// A Bank's position in the Target's list, which is the order a Project will
/// one day take a prefix of.
struct BankIndex
{
  std::uint32_t value = 0;

  friend bool operator==( BankIndex, BankIndex ) = default;
};

/// A unit set's position in the Target's list, which is declaration order.
struct UnitSetIndex
{
  std::uint32_t value = 0;

  friend bool operator==( UnitSetIndex, UnitSetIndex ) = default;
};

/// A Window's position in the Target's list, which is declaration order.
struct WindowIndex
{
  std::uint32_t value = 0;

  friend bool operator==( WindowIndex, WindowIndex ) = default;
};

/// A Pane's position in the Target's list, which is declaration order.
struct PaneIndex
{
  std::uint32_t value = 0;

  friend bool operator==( PaneIndex, PaneIndex ) = default;
};

/// A half-open range of addresses.
struct AddressRange
{
  std::uint32_t begin = 0;
  std::uint32_t end = 0;

  [[nodiscard]] std::uint32_t size() const
  {
    return end - begin;
  }

  /// Whether the two hold an address in common: a range of nothing holds
  /// none, and two that only touch share nothing.
  [[nodiscard]] bool overlaps( AddressRange other ) const
  {
    return begin < other.end && other.begin < end;
  }
};

} // namespace nga::model
