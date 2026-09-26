#include "nga/model/MemoryMap.hpp"

#include "nga/model/Prune.hpp"
#include "nga/model/Transition.hpp"

#include <spdlog/fmt/fmt.h>

#include <algorithm>
#include <string_view>

namespace nga::model
{

namespace
{

std::string hexOf( std::uint32_t value )
{
  return fmt::format( "${:04X}", value );
}

std::string kindOf( MapEntry const& entry )
{
  std::string kind;
  switch ( entry.kind )
  {
  case SectionKind::PROC:
    kind = "proc";
    break;
  case SectionKind::TEMPORARY:
    kind = "temporary";
    break;
  case SectionKind::PLAIN:
    kind = "section";
    break;
  }
  if ( entry.movable )
  {
    kind += " movable";
  }
  if ( entry.root )
  {
    kind += " root";
  }
  return kind;
}

/// The first and last Phase of an entry's run, as numbers; zero for an entry
/// in no Phase, which every caller asks about first.
std::uint32_t firstOf( MapEntry const& entry )
{
  return entry.firstPhase.value_or( PhaseIndex{} ).value;
}

std::uint32_t lastOf( MapEntry const& entry )
{
  return entry.lastPhase.value_or( PhaseIndex{} ).value;
}

SectionKind kindOfSection( Section const& section )
{
  if ( section.isProc() )
  {
    return SectionKind::PROC;
  }
  return section.isTemporary() ? SectionKind::TEMPORARY : SectionKind::PLAIN;
}

/// The first and last Phase of a Residency, or nothing for one of none.
std::pair<std::optional<PhaseIndex>, std::optional<PhaseIndex>> spanOf( Residency const& residency )
{
  std::optional<PhaseIndex> first;
  std::optional<PhaseIndex> last;
  for ( std::uint32_t phase = 0; phase < residency.phaseCount(); ++phase )
  {
    if ( residency.includes( PhaseIndex{ phase } ) )
    {
      first = first.has_value() ? first : std::optional{ PhaseIndex{ phase } };
      last = PhaseIndex{ phase };
    }
  }
  return { first, last };
}

/// "  live intro..game", or nothing for an image live nowhere.
std::string liveText( MemoryMap const& map, std::optional<PhaseIndex> first, std::optional<PhaseIndex> last )
{
  if ( !first.has_value() || !last.has_value() )
  {
    return "";
  }
  std::string const from = map.phases[first->value].name;
  std::string const to = map.phases[last->value].name;
  return "  live " + ( from == to ? from : from + ".." + to );
}

std::string phasesOf( MemoryMap const& map, MapEntry const& entry )
{
  if ( !entry.firstPhase.has_value() || !entry.lastPhase.has_value() )
  {
    return "-";
  }
  std::string const first = map.phases[firstOf( entry )].name;
  std::string const last = map.phases[lastOf( entry )].name;
  return first == last ? first : first + ".." + last;
}

bool overlaps( MapEntry const& a, MapEntry const& b )
{
  bool const addresses = a.begin < b.end && b.begin < a.end;
  if ( !addresses )
  {
    return false;
  }
  if ( !a.firstPhase.has_value() || !b.firstPhase.has_value() )
  {
    return true;
  }
  return firstOf( a ) <= lastOf( b ) && firstOf( b ) <= lastOf( a );
}

} // namespace

MemoryMap memoryMapOf( Patched const& build )
{
  MemoryMap map;
  PhaseGraph const& graph = build.phases();
  Layout const& layout = build.layout();
  Sizes const& sizes = build.sizes();
  Storage const& storage = build.storage();
  Target const& target = build.target();
  std::span<Module const> const modules = build.modules();

  // A Slot's Cell is a Section the tool built and gave no Label, so the map
  // would have nothing to call it. It has the Slot's name to call it by, and
  // with several Slots that is the only thing telling the Cells apart. Not a
  // Label, because a Slot that takes arguments owns a Namespace of its name
  // and a Symbol of a Namespace's name is refused.
  std::vector<std::pair<SectionRef, std::string_view>> cellNames;
  for ( Module const& one : modules )
  {
    for ( Symbol const& symbol : one.symbols().symbols() )
    {
      auto const* const slot = std::get_if<SlotDeclaration>( &symbol.value );
      if ( slot != nullptr && slot->cell.has_value() )
      {
        cellNames.emplace_back( *slot->cell, symbol.name );
      }
    }
  }
  auto const cellNamed = [&cellNames]( SectionRef where ) -> std::optional<std::string_view>
  {
    auto const found = std::ranges::find_if( cellNames, [where]( auto const& one ) { return one.first == where; } );
    return found == cellNames.end() ? std::nullopt : std::optional{ found->second };
  };

  map.pools = target.pools;
  map.bankSize = target.bankSize();
  map.unitWord = target.storageUnits.has_value() ? "bank" : "unit";
  for ( std::uint32_t phase = 0; phase < graph.phases.size(); ++phase )
  {
    map.phases.push_back( MapPhase{ .name = graph.phases[phase].name.value_or( "phase" + std::to_string( phase ) ) } );
  }
  map.banks.resize( target.unitCount );

  for ( std::uint32_t module = 0; module < modules.size(); ++module )
  {
    Module const& one = modules[module];
    for ( std::uint32_t index = 0; index < one.sections().size(); ++index )
    {
      SectionRef const where{ .module = ModuleIndex{ module }, .section = SectionIndex{ index } };
      Section const& section = one.sections()[index];
      if ( !build.reachable().includes( where ) || !layout.isPlaced( where ) || !sizes.isKnown( where ) ||
           sizes.sizeOfSection( where ) == 0 )
      {
        continue;
      }
      std::uint32_t const size = sizes.sizeOfSection( where );
      auto const entryAt = [&]( std::optional<PhaseIndex> first, std::optional<PhaseIndex> last, std::uint32_t at )
      {
        MapEntry entry{ .where = where,
                        .name = cellNamed( where )
                                    .transform( []( std::string_view name ) { return std::string{ name }; } )
                                    .value_or( one.displayNameOf( where.section, build.sources() ) ),
                        .module = std::string{ one.name() },
                        .firstPhase = first,
                        .lastPhase = last,
                        .begin = at,
                        .end = at + size,
                        .placement = section.placement(),
                        .kind = kindOfSection( section ),
                        .root = section.isRoot(),
                        .movable = section.isMovable(),
                        .waits = std::nullopt,
                        .storedSize = 0,
                        .liveFirst = std::nullopt,
                        .liveLast = std::nullopt,
                        .transform = {},
                        .shares = {},
                        .pane = {},
                        .paneState = 0 };
        if ( section.pane().has_value() )
        {
          entry.pane = target.panes[section.pane()->value].name;
          entry.paneState = layout.stateOfPane( *section.pane() ).value_or( 0 );
        }
        if ( storage.hasPayload( where ) && storage.isPlaced( where ) )
        {
          entry.waits = storage.addressOf( where );
          entry.storedSize = storage.sizeOf( where );
          std::tie( entry.liveFirst, entry.liveLast ) = spanOf( storage.liveOf( where ) );
          std::uint8_t const transform = storage.transformOf( where );
          entry.transform = transform < build.project().decoders.size() ? build.project().decoders[transform].format
                                                                        : "transform " + std::to_string( transform );
        }
        map.entries.push_back( std::move( entry ) );
      };

      // Runs of consecutive Phases at one address: a Section that is not
      // Movable is one run per gap in its Residency, a Movable one splits
      // where it moved.
      Residency const& residency = one.residency();
      std::optional<PhaseIndex> first;
      std::uint32_t at = 0;
      for ( std::uint32_t phase = 0; phase < graph.phases.size(); ++phase )
      {
        PhaseIndex const here{ phase };
        bool const present = residency.phaseCount() > phase && residency.includes( here );
        std::uint32_t const address = present ? layout.addressIn( where, here ) : 0;
        if ( first.has_value() && ( !present || address != at ) )
        {
          entryAt( first, PhaseIndex{ phase - 1 }, at );
          first.reset();
        }
        if ( present && !first.has_value() )
        {
          first = here;
          at = address;
        }
      }
      if ( first.has_value() )
      {
        entryAt( first, PhaseIndex{ static_cast<std::uint32_t>( graph.phases.size() - 1 ) }, at );
      }
      else if ( residency.isEmpty() )
      {
        entryAt( std::nullopt, std::nullopt, layout.addressOf( where ) );
      }
    }
  }

  std::ranges::stable_sort( map.entries,
                            []( MapEntry const& a, MapEntry const& b )
                            {
                              std::uint32_t const phaseA = a.firstPhase.has_value() ? firstOf( a ) : ~0U;
                              std::uint32_t const phaseB = b.firstPhase.has_value() ? firstOf( b ) : ~0U;
                              if ( phaseA != phaseB )
                              {
                                return phaseA < phaseB;
                              }
                              if ( a.begin != b.begin )
                              {
                                return a.begin < b.begin;
                              }
                              return a.name < b.name;
                            } );

  // Temporaries at one address in one Phase: what Trace bought.
  for ( std::size_t a = 0; a < map.entries.size(); ++a )
  {
    for ( std::size_t b = 0; b < map.entries.size(); ++b )
    {
      if ( a != b && map.entries[a].kind == SectionKind::TEMPORARY && map.entries[b].kind == SectionKind::TEMPORARY &&
           overlaps( map.entries[a], map.entries[b] ) )
      {
        map.entries[a].shares.push_back( map.entries[b].name );
      }
    }
  }

  // What each Phase takes of each pool, over the Sections present in it. The
  // Temporaries at one address take its bytes once, which is what sharing
  // them is for; only a Temporary shares, so every other Section adds its
  // size.
  struct Shared
  {
    std::vector<std::pair<std::uint32_t, std::uint32_t>> zeroPage;
    std::vector<std::pair<std::uint32_t, std::uint32_t>> general;
  };

  // A Section standing in the ROM pool is in neither: the general pool is RAM,
  // and what ROM comes to is one number for the whole run.
  auto const inRom = [&map]( MapEntry const& entry )
  {
    return std::ranges::any_of( map.pools.readOnly,
                                [&entry]( AddressRange const& range )
                                { return entry.begin >= range.begin && entry.begin < range.end; } );
  };

  std::vector<Shared> temporaries( map.phases.size() );
  std::vector<std::pair<std::uint32_t, std::uint32_t>> rom;
  for ( MapEntry const& entry : map.entries )
  {
    if ( inRom( entry ) )
    {
      rom.emplace_back( entry.begin, entry.end );
      continue;
    }
    if ( !entry.firstPhase.has_value() || !entry.lastPhase.has_value() )
    {
      continue;
    }
    bool const zeroPage = entry.placement == PlacementClass::ZEROPAGE;
    for ( std::uint32_t phase = firstOf( entry ); phase <= lastOf( entry ); ++phase )
    {
      if ( entry.kind == SectionKind::TEMPORARY )
      {
        ( zeroPage ? temporaries[phase].zeroPage : temporaries[phase].general ).emplace_back( entry.begin, entry.end );
        continue;
      }
      ( zeroPage ? map.phases[phase].zeroPageUsed : map.phases[phase].generalUsed ) += entry.end - entry.begin;
    }
  }
  auto const covered = []( std::vector<std::pair<std::uint32_t, std::uint32_t>> ranges )
  {
    std::ranges::sort( ranges );
    std::uint32_t bytes = 0;
    std::uint32_t reached = 0;
    for ( auto const& [begin, end] : ranges )
    {
      std::uint32_t const from = std::max( begin, reached );
      if ( end > from )
      {
        bytes += end - from;
        reached = end;
      }
    }
    return bytes;
  };
  for ( std::size_t phase = 0; phase < map.phases.size(); ++phase )
  {
    map.phases[phase].zeroPageUsed += covered( std::move( temporaries[phase].zeroPage ) );
    map.phases[phase].generalUsed += covered( std::move( temporaries[phase].general ) );
  }
  // A Movable Section is not read-only and so is in no ROM pool, but the union
  // is what the number means whatever the entries turn out to be.
  map.romUsed = covered( std::move( rom ) );

  // Storage: every Frame, and what every Bank came to.
  for ( std::uint32_t index = 0; index < storage.frameCount(); ++index )
  {
    FrameIndex const frame{ index };
    if ( !storage.isFramePlaced( frame ) )
    {
      continue;
    }
    auto const [first, last] = spanOf( storage.frameLiveOf( frame ) );
    map.frames.push_back( MapFrame{ .name = nameOfFrame( graph, storage.frameFrom( frame ), storage.frameTo( frame ) ),
                                    .waits = storage.frameAddressOf( frame ),
                                    .size = storage.frameSizeOf( frame ),
                                    .liveFirst = first,
                                    .liveLast = last } );
  }
  // What each unit came to: an image may run from one unit into the next,
  // and counts against every unit it touches.
  auto const account = [&map, &target]( StorageAddress at, std::uint32_t size )
  {
    std::uint32_t const begin = target.positionOf( at );
    for ( std::uint32_t unit = at.bank.value; unit < map.banks.size() && unit * map.bankSize < begin + size; ++unit )
    {
      std::uint32_t const within = std::min( map.bankSize, begin + size - ( unit * map.bankSize ) );
      map.banks[unit].used = std::max( map.banks[unit].used, within );
    }
  };
  for ( MapFrame const& frame : map.frames )
  {
    account( frame.waits, frame.size );
  }
  for ( MapEntry const& entry : map.entries )
  {
    if ( entry.waits.has_value() )
    {
      account( entry.waits.value_or( StorageAddress{} ), entry.storedSize );
    }
  }
  return map;
}

std::string renderMapText( MemoryMap const& map )
{
  std::string out = "NGA memory map\n";
  std::uint32_t const zeroPageSize = map.pools.zeroPageSize();
  std::uint32_t const generalSize = map.pools.generalSize();
  std::uint32_t const romSize = map.pools.readOnlySize();

  // Once, and not per Phase, and only where the Target has ROM at all, so that
  // a machine without a cartridge reads as it always did.
  if ( romSize > 0 )
  {
    out += fmt::format( "\nrom: {} of {} bytes\n", map.romUsed, romSize );
  }

  // Columns as wide as the longest name, so that every line reads.
  std::size_t width = 0;
  for ( MapEntry const& entry : map.entries )
  {
    width = std::max( width, entry.module.size() + 1 + entry.name.size() );
  }

  for ( std::uint32_t phase = 0; phase < map.phases.size(); ++phase )
  {
    MapPhase const& one = map.phases[phase];
    out += fmt::format( "\nphase {} ({})\n", one.name, phase );
    out += fmt::format( "  zero page: {} of {} bytes\n", one.zeroPageUsed, zeroPageSize );
    out += fmt::format( "  memory:    {} of {} bytes\n", one.generalUsed, generalSize );
    std::vector<MapEntry const*> present;
    for ( MapEntry const& entry : map.entries )
    {
      if ( entry.firstPhase.has_value() && firstOf( entry ) <= phase && lastOf( entry ) >= phase )
      {
        present.push_back( &entry );
      }
    }
    std::ranges::stable_sort( present,
                              []( MapEntry const* a, MapEntry const* b )
                              { return a->begin != b->begin ? a->begin < b->begin : a->name < b->name; } );
    for ( MapEntry const* entry : present )
    {
      std::string line = fmt::format( "  {}-{}  {:<{}} {:<18} {}",
                                      hexOf( entry->begin ),
                                      hexOf( entry->end - 1 ),
                                      entry->module + "." + entry->name,
                                      width,
                                      kindOf( *entry ),
                                      phasesOf( map, *entry ) );
      if ( entry->waits.has_value() )
      {
        line += fmt::format( "  waits in {} {} at {} ({}, {} bytes)",
                             map.unitWord,
                             entry->waits.value_or( StorageAddress{} ).bank.value,
                             hexOf( entry->waits.value_or( StorageAddress{} ).offset ),
                             entry->transform,
                             entry->storedSize );
      }
      if ( !entry->pane.empty() )
      {
        line += fmt::format( "  in pane {} (state {})", entry->pane, entry->paneState );
      }
      if ( !entry->shares.empty() )
      {
        line += "  shares with";
        for ( std::string const& other : entry->shares )
        {
          line += " " + other;
        }
      }
      out += line + "\n";
    }
  }

  std::vector<MapEntry const*> loose;
  for ( MapEntry const& entry : map.entries )
  {
    if ( !entry.firstPhase.has_value() )
    {
      loose.push_back( &entry );
    }
  }
  if ( !loose.empty() )
  {
    out += "\nin no phase\n";
    for ( MapEntry const* entry : loose )
    {
      out += fmt::format( "  {}-{}  {:<{}} {}\n",
                          hexOf( entry->begin ),
                          hexOf( entry->end - 1 ),
                          entry->module + "." + entry->name,
                          width,
                          kindOf( *entry ) );
    }
  }

  if ( !map.banks.empty() )
  {
    out += "\nstorage\n";
    for ( std::uint32_t bank = 0; bank < map.banks.size(); ++bank )
    {
      out += fmt::format( "  {} {}: {} of {} bytes\n", map.unitWord, bank, map.banks[bank].used, map.bankSize );
      std::vector<std::pair<std::uint32_t, std::string>> lines;
      for ( MapFrame const& frame : map.frames )
      {
        if ( frame.waits.bank.value == bank )
        {
          lines.emplace_back( frame.waits.offset,
                              fmt::format( "    {}-{}  {:<{}}{}",
                                           hexOf( frame.waits.offset ),
                                           hexOf( frame.waits.offset + frame.size - 1 ),
                                           frame.name,
                                           width,
                                           liveText( map, frame.liveFirst, frame.liveLast ) ) );
        }
      }
      for ( MapEntry const& entry : map.entries )
      {
        // A Section's Payload waits once, however many runs the Section has.
        if ( entry.waits.has_value() && entry.waits.value_or( StorageAddress{} ).bank.value == bank &&
             entry.firstPhase.has_value() &&
             std::ranges::none_of( lines,
                                   [&entry]( auto const& line )
                                   { return line.first == entry.waits.value_or( StorageAddress{} ).offset; } ) )
        {
          lines.emplace_back(
              entry.waits.value_or( StorageAddress{} ).offset,
              fmt::format( "    {}-{}  {:<{}} {} ({} bytes){}",
                           hexOf( entry.waits.value_or( StorageAddress{} ).offset ),
                           hexOf( entry.waits.value_or( StorageAddress{} ).offset + entry.storedSize - 1 ),
                           entry.module + "." + entry.name,
                           width,
                           entry.transform,
                           entry.storedSize,
                           liveText( map, entry.liveFirst, entry.liveLast ) ) );
        }
      }
      std::ranges::stable_sort( lines, []( auto const& a, auto const& b ) { return a.first < b.first; } );
      for ( auto const& [offset, line] : lines )
      {
        out += line + "\n";
      }
    }
  }
  return out;
}

} // namespace nga::model
