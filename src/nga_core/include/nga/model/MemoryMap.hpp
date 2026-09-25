#pragma once

#include "nga/model/Build.hpp"
#include "nga/model/Chunk.hpp"
#include "nga/model/PlacementClass.hpp"
#include "nga/model/Project.hpp"
#include "nga/model/Section.hpp"
#include "nga/model/Storage.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace nga::model
{

/// One Section as it stands across a run of consecutive Phases at one
/// address: a Section present in Phases 0 to 2 is one of these, a Movable
/// one standing elsewhere in Phase 1 is three.
struct MapEntry
{
  SectionRef where;
  std::string name;
  std::string module;

  /// The first and last Phase of the run, inclusive; nothing for a Section
  /// in no Phase, which stands somewhere all the same.
  std::optional<PhaseIndex> firstPhase;
  std::optional<PhaseIndex> lastPhase;

  /// Half-open.
  std::uint32_t begin = 0;
  std::uint32_t end = 0;

  PlacementClass placement = PlacementClass::ABSOLUTE;
  SectionKind kind = SectionKind::PLAIN;
  bool root = false;
  bool movable = false;

  /// Where the Section's Payload waits, when it has one, and how it waits,
  /// and the first and last Phase it is live in — see the glossary.
  std::optional<StorageAddress> waits;
  std::uint32_t storedSize = 0;
  std::optional<PhaseIndex> liveFirst;
  std::optional<PhaseIndex> liveLast;
  /// The format the Payload waits as, by name; empty where it waits nowhere.
  std::string transform;

  /// The Temporaries this one shares an address with in some Phase of the
  /// run, by name — the whole point of a Temporary, made visible.
  std::vector<std::string> shares;

  /// The Pane the Section is in, by name, and the state its Window shows it
  /// in; empty for a Section in `fixed` or a base state.
  std::string pane{};
  std::uint32_t paneState = 0;
};

/// One Frame as it waits.
struct MapFrame
{
  std::string name;
  StorageAddress waits;
  std::uint32_t size = 0;
  std::optional<PhaseIndex> liveFirst;
  std::optional<PhaseIndex> liveLast;
};

/// What a Phase takes of each pool: the bytes its Sections occupy, a byte
/// that Temporaries share counted once.
struct MapPhase
{
  std::string name;
  std::uint32_t zeroPageUsed = 0;
  std::uint32_t generalUsed = 0;
};

/// What a Bank holds, as bytes used of its size.
struct MapBank
{
  std::uint32_t used = 0;
};

/// The memory map: every placed Section that Prune kept, Phase by Phase and
/// address by address, and everything that waits in a Bank. A report read
/// off the Layout and Storage, deciding nothing — see
/// docs/spec/memory-map.md.
struct MemoryMap
{
  std::vector<MapPhase> phases;
  std::vector<MapEntry> entries;
  std::vector<MapFrame> frames;
  std::vector<MapBank> banks;
  Pools pools;
  std::uint32_t bankSize = 0;

  /// What one unit of storage is called here: a **Bank** where storage is a
  /// unit set, since that is what a Bank is, and a plain **unit** where
  /// storage is by count — a diskette has no unit set, and the glossary
  /// refuses `bank` as a general word. See docs/spec/glossary.md.
  std::string_view unitWord = "bank";
};

/// Reads the map off a finished build. Deterministic: entries are ordered by
/// first Phase, then address, then name.
MemoryMap memoryMapOf( Patched const& build );

/// The map as text, Phase by Phase — see docs/spec/memory-map.md.
std::string renderMapText( MemoryMap const& map );

} // namespace nga::model
