#pragma once

#include "nga/model/Build.hpp"
#include "nga/model/Chunk.hpp"
#include "nga/model/Project.hpp"
#include "nga/model/Section.hpp"
#include "nga/model/Storage.hpp"
#include "nga/model/Symbols.hpp"
#include "nga/syntax/Statement.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace nga::model
{

/// Where one stored form waits, and how long it is.
struct MapPlacement
{
  StorageAddress at;
  std::uint32_t size = 0;
};

/// One Section as it stands across a run of consecutive Phases: a Section
/// present in Phases 0 to 2 is one of these, one held in 0 and 2 alone is two.
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

  /// What of it holds initialised bytes, half-open and in the space `begin`
  /// and `end` are in; empty, at `begin`, for a Section of reservations
  /// alone. Reserved space at either end lies outside it — see
  /// docs/decisions/0026-a-payload-is-the-initialised-extent.md.
  std::uint32_t initialisedBegin = 0;
  std::uint32_t initialisedEnd = 0;

  /// The Constraints its text wrote, so that a reader can tell what was asked
  /// for from what the solver chose: `at`, `align`, `within`, the Proc
  /// `.endp then` falls into, by name, and the Regions `in` named, by name.
  /// Read by the facts alone.
  bool pinned = false;
  std::optional<std::uint32_t> alignment;
  std::optional<std::uint32_t> boundary;
  std::string then{};
  std::vector<std::string> regions{};

  syntax::PlacementClass placement = syntax::PlacementClass::ABSOLUTE;
  SectionKind kind = SectionKind::PLAIN;
  bool root = false;

  /// Held in Phases that do not need it, because nothing could bring its
  /// bytes back: its Phases are the ones it is live in, not the ones that
  /// need it — see heldAcross.
  bool kept = false;

  /// Its bytes lie under a ROM while the entry Phase has it in, and the load
  /// moved them there — see
  /// docs/decisions/0241-bytes-kept-for-a-later-phase-may-lie-hidden-under-a-rom.md.
  bool hidden = false;

  /// Where the Section's Payload waits, when it has one, and how it waits,
  /// and the first and last Phase it is live in — see the glossary.
  ///
  /// **One entry unless no unit of storage held the whole of it**, and then one
  /// for each form it was cut into: they are placed apart and a run from the
  /// first to the last would be a stretch of the medium that is not the
  /// Payload's at all.
  std::vector<MapPlacement> waits;
  std::optional<PhaseIndex> liveFirst;
  std::optional<PhaseIndex> liveLast;
  /// The format the Payload waits as, by name; empty where it waits nowhere.
  std::string transform;

  /// The Temporaries this one shares an address with in some Phase of the
  /// run, by name — the whole point of a Temporary, made visible.
  std::vector<std::string> shares;

  /// The Pane the Section is in, by name, and the state its Window shows it
  /// in; empty for a Section in `fixed`.
  std::string pane{};
  std::uint32_t paneState = 0;

  /// The unit set the Section stands in as the medium sees it, by name, and
  /// then `begin` and `end` are linear addresses; empty for any other — see
  /// docs/decisions/0254-a-section-may-stand-in-a-unit-set.md.
  std::string units{};
};

/// A Section Prune dropped: it stands nowhere and waits nowhere, and what is
/// left of it is where it was written.
struct MapPruned
{
  std::string name;
  std::string module;
  SectionKind kind = SectionKind::PLAIN;

  /// Its own text, from the directive that opened it to the one that closed
  /// it, less every Section written inside it, which is listed for itself if
  /// it was dropped too and stands somewhere if it was not. Empty for a
  /// Section the tool built and wrote no text for.
  std::vector<SymbolSite> text;
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

  /// What Prune dropped, Module by Module in the order it was written. Read
  /// by the facts alone: the text map is a map of memory, and these stand in
  /// none of it.
  std::vector<MapPruned> pruned;

  std::vector<MapFrame> frames;
  std::vector<MapBank> banks;
  Pools pools;
  std::uint32_t bankSize = 0;

  /// What the program takes of the ROM pool, over the whole run and not per
  /// Phase: nothing loads ROM, so two Sections cannot share an address there
  /// and every Phase's Sections add up in one space — see
  /// docs/decisions/0215-a-cartridge-is-rom-and-a-section-stands-in-it-when-nothing-writes-it.md.
  std::uint32_t romUsed = 0;

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
