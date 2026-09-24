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

/// Text made safe inside HTML and inside a JavaScript string.
std::string escaped( std::string_view text )
{
  std::string out;
  for ( char const c : text )
  {
    switch ( c )
    {
    case '&':
      out += "&amp;";
      break;
    case '<':
      out += "&lt;";
      break;
    case '>':
      out += "&gt;";
      break;
    case '"':
      out += "&quot;";
      break;
    case '\\':
      out += "\\\\";
      break;
    default:
      out += c;
    }
  }
  return out;
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

  std::vector<Shared> temporaries( map.phases.size() );
  for ( MapEntry const& entry : map.entries )
  {
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

std::string renderMapHtml( MemoryMap const& map )
{
  std::string out;
  out += "<!DOCTYPE html>\n<html>\n<head>\n<meta charset=\"UTF-8\">\n<title>NGA memory map</title>\n";
  out += R"css(<style>
  body { background: #111; color: #ddd; font-family: monospace; font-size: 12px; margin: 0; padding: 12px; }
  h2 { font-size: 13px; margin: 18px 0 6px; color: #aaa; font-weight: normal; }
  .panel { position: relative; background: #1a1a1a; border: 1px solid #333; overflow: hidden; }
  .pool { position: absolute; top: 0; bottom: 0; background: #232323; }
  .row { position: absolute; left: 0; right: 0; border-top: 1px solid #2a2a2a; color: #666; padding-left: 4px; box-sizing: border-box; }
  .rect { position: absolute; box-sizing: border-box; border: 1px solid #fff8; opacity: 0.75; }
  .rect:hover { opacity: 1; border: 2px solid #fff; z-index: 2; }
  .tip { position: absolute; display: none; background: #000; color: #fff; padding: 6px 8px; border: 1px solid #888; pointer-events: none; z-index: 10; white-space: pre; }
  .tick { position: absolute; top: 0; color: #777; font-size: 10px; border-left: 1px solid #333; padding-left: 2px; height: 100%; }
</style>
</head>
<body>
<h1 style="font-size:15px;font-weight:normal;margin:0 0 8px">NGA memory map</h1>
<div class="tip" id="tip"></div>
)css";
  out += "<script>\nconst phases = [";
  for ( MapPhase const& phase : map.phases )
  {
    out += fmt::format( "{{name:\"{}\",zp:{},mem:{}}},", escaped( phase.name ), phase.zeroPageUsed, phase.generalUsed );
  }
  out += "];\nconst entries = [";
  for ( MapEntry const& entry : map.entries )
  {
    std::string shares;
    for ( std::string const& other : entry.shares )
    {
      shares += escaped( other ) + " ";
    }
    out +=
        fmt::format( "{{name:\"{}\",module:\"{}\",first:{},last:{},begin:{},end:{},zp:{},kind:\"{}\",waits:{},bank:{},"
                     "offset:{},stored:{},transform:\"{}\",shares:\"{}\",live:\"{}\"}},",
                     escaped( entry.name ),
                     escaped( entry.module ),
                     entry.firstPhase.has_value() ? static_cast<int>( firstOf( entry ) ) : -1,
                     entry.lastPhase.has_value() ? static_cast<int>( lastOf( entry ) ) : -1,
                     entry.begin,
                     entry.end,
                     entry.placement == PlacementClass::ZEROPAGE ? "true" : "false",
                     kindOf( entry ),
                     entry.waits.has_value() ? "true" : "false",
                     entry.waits.has_value() ? entry.waits.value_or( StorageAddress{} ).bank.value : 0,
                     entry.waits.has_value() ? entry.waits.value_or( StorageAddress{} ).offset : 0,
                     entry.storedSize,
                     escaped( entry.transform ),
                     shares,
                     escaped( liveText( map, entry.liveFirst, entry.liveLast ) ) );
  }
  out += "];\nconst frames = [";
  for ( MapFrame const& frame : map.frames )
  {
    out += fmt::format( R"({{name:"{}",bank:{},offset:{},size:{},live:"{}"}},)",
                        escaped( frame.name ),
                        frame.waits.bank.value,
                        frame.waits.offset,
                        frame.size,
                        escaped( liveText( map, frame.liveFirst, frame.liveLast ) ) );
  }
  out += fmt::format( "];\nconst banks = [" );
  for ( MapBank const& bank : map.banks )
  {
    out += fmt::format( "{{used:{}}},", bank.used );
  }
  auto const rangesOf = []( std::span<AddressRange const> ranges )
  {
    std::string list;
    for ( AddressRange const& range : ranges )
    {
      list += fmt::format( "[{},{}],", range.begin, range.end );
    }
    return list;
  };
  out += fmt::format( "];\nconst pools = {{zp:[{}],mem:[{}],zpSize:{},memSize:{}}};\nconst bankSize = {};\n",
                      rangesOf( map.pools.zeroPage ),
                      rangesOf( map.pools.general ),
                      map.pools.zeroPageSize(),
                      map.pools.generalSize(),
                      map.bankSize );
  out += R"js(
const hex = (v, d) => "$" + v.toString(16).toUpperCase().padStart(d, "0");
const colours = ["#4a7bd0", "#4fae6a", "#d08a3a", "#b05fc4", "#c4504f", "#3aaeae", "#c9b83a", "#8f8f8f", "#7a5fd0", "#5faf8f"];
const moduleColour = new Map();
const colourOf = m => { if (!moduleColour.has(m)) moduleColour.set(m, colours[moduleColour.size % colours.length]); return moduleColour.get(m); };
const tip = document.getElementById("tip");
const ROW = 26;
const BLOCKS = 8;
const NARROWEST = 16;
const width = Math.max(600, document.body.clientWidth - 40);

// A panel shows a range of addresses across and a row per Phase or Bank
// down. A click zooms into the block under the pointer, one of BLOCKS across
// the current range, until a range is NARROWEST bytes wide; the buttons go
// back one step and all the way out.
function panel(title, rows, lo, hi, pools) {
  const p = { title, rows, full: [lo, hi], lo, hi, stack: [], pools, rects: [] };
  const head = document.createElement("h2");
  const label = document.createElement("span");
  const out = document.createElement("button"); out.textContent = "zoom out";
  const all = document.createElement("button"); all.textContent = "all";
  for (const b of [out, all]) { b.style.marginLeft = "8px"; b.style.font = "inherit"; b.style.background = "#222"; b.style.color = "#ddd"; b.style.border = "1px solid #555"; b.style.cursor = "pointer"; }
  head.appendChild(label); head.appendChild(out); head.appendChild(all);
  document.body.appendChild(head);
  const div = document.createElement("div"); div.className = "panel";
  div.style.width = width + "px"; div.style.height = (rows.length * ROW) + "px"; div.style.cursor = "zoom-in";
  document.body.appendChild(div);
  p.div = div; p.label = label;
  out.addEventListener("click", () => { if (p.stack.length) { [p.lo, p.hi] = p.stack.pop(); render(p); } });
  all.addEventListener("click", () => { p.stack = []; [p.lo, p.hi] = p.full; render(p); });
  div.addEventListener("click", e => {
    const span = p.hi - p.lo;
    if (span <= NARROWEST) return;
    // From the panel's left edge: a click lands on a rectangle as often as
    // on the panel, and an offset from the rectangle would zoom elsewhere.
    const at = p.lo + ((e.clientX - div.getBoundingClientRect().left) / width) * span;
    const block = Math.max(NARROWEST, span / BLOCKS);
    const lo = Math.floor((at - p.lo) / block) * block + p.lo;
    p.stack.push([p.lo, p.hi]);
    p.lo = lo; p.hi = Math.min(lo + block, p.full[1]);
    render(p);
  });
  return p;
}

function render(p) {
  const div = p.div;
  while (div.firstChild) div.removeChild(div.firstChild);
  const span = p.hi - p.lo;
  const scale = width / span;
  p.label.textContent = p.title + "  " + hex(p.lo, 4) + "-" + hex(p.hi - 1, 4) + (span <= NARROWEST ? "" : "  (click a block to zoom)");
  for (const [a, b] of p.pools) {
    const lo = Math.max(a, p.lo), hi = Math.min(b, p.hi);
    if (hi <= lo) continue;
    const pool = document.createElement("div"); pool.className = "pool";
    pool.style.left = ((lo - p.lo) * scale) + "px"; pool.style.width = ((hi - lo) * scale) + "px";
    div.appendChild(pool);
  }
  const block = span > NARROWEST ? Math.max(NARROWEST, span / BLOCKS) : span / 8;
  for (let t = p.lo; t < p.hi; t += block) {
    const tick = document.createElement("div"); tick.className = "tick";
    tick.style.left = ((t - p.lo) * scale) + "px"; tick.textContent = hex(Math.round(t), 4); div.appendChild(tick);
  }
  p.rows.forEach((label, i) => {
    const row = document.createElement("div"); row.className = "row";
    row.style.top = (i * ROW) + "px"; row.style.height = ROW + "px"; row.style.lineHeight = ROW + "px";
    row.textContent = label; div.appendChild(row);
  });
  for (const r of p.rects) {
    const x1 = Math.max(r.x1, p.lo), x2 = Math.min(r.x2, p.hi);
    if (x2 <= x1) continue;
    const d = document.createElement("div"); d.className = "rect";
    d.style.left = ((x1 - p.lo) * scale) + "px"; d.style.width = Math.max(2, (x2 - x1) * scale) + "px";
    d.style.top = (r.r1 * ROW + 3) + "px"; d.style.height = ((r.r2 - r.r1 + 1) * ROW - 6) + "px";
    d.style.background = r.colour;
    d.addEventListener("mousemove", e => { tip.style.display = "block"; tip.textContent = r.text; tip.style.left = (e.pageX + 12) + "px"; tip.style.top = (e.pageY + 12) + "px"; });
    d.addEventListener("mouseleave", () => { tip.style.display = "none"; });
    div.appendChild(d);
  }
}

function rect(p, x1, x2, r1, r2, colour, text) { p.rects.push({ x1, x2, r1, r2, colour, text }); }

const phaseRows = phases.map((p, i) => p.name + "  zp " + p.zp + "/" + pools.zpSize + "  mem " + p.mem + "/" + pools.memSize);
const describe = e => e.module + "." + e.name + "\n" + e.kind + "\nphases: " + (e.first < 0 ? "-" : (e.first === e.last ? phases[e.first].name : phases[e.first].name + ".." + phases[e.last].name)) + "\naddress: " + hex(e.begin, 4) + "-" + hex(e.end - 1, 4) + "\nsize: " + (e.end - e.begin) + (e.waits ? "\nwaits in bank " + e.bank + " at " + hex(e.offset, 4) + " (" + e.transform + ", " + e.stored + " bytes)" : "") + (e.shares ? "\nshares with " + e.shares : "");

const zp = panel("zero page", phaseRows, 0, 256, pools.zp);
const mem = panel("memory", phaseRows, 0, 65536, pools.mem);
for (const e of entries) {
  if (e.first < 0) continue;
  rect(e.zp ? zp : mem, e.begin, e.end, e.first, e.last, colourOf(e.module), describe(e));
}
render(zp); render(mem);
if (banks.length) {
  const bankRows = banks.map((b, i) => "bank " + i + "  " + b.used + "/" + bankSize);
  const st = panel("storage", bankRows, 0, bankSize, []);
  // An image may run from one unit into the next: one rectangle per unit it touches.
  const stored = (bank, offset, size, colour, text) => {
    for (let b = bank, o = offset, left = size; left > 0; b++, o = 0) {
      const n = Math.min(left, bankSize - o);
      rect(st, o, o + n, b, b, colour, text);
      left -= n;
    }
  };
  for (const f of frames) stored(f.bank, f.offset, f.size, "#666", f.name + "\noffset: " + hex(f.offset, 4) + "\nsize: " + f.size + (f.live ? "\n" + f.live.trim() : ""));
  const seen = new Set();
  for (const e of entries) {
    if (!e.waits || seen.has(e.module + "." + e.name)) continue;
    seen.add(e.module + "." + e.name);
    stored(e.bank, e.offset, e.stored, colourOf(e.module), e.module + "." + e.name + "\n" + e.transform + ", " + e.stored + " bytes\noffset: " + hex(e.offset, 4) + (e.live ? "\n" + e.live.trim() : ""));
  }
  render(st);
}
</script>
</body>
</html>
)js";
  return out;
}

} // namespace nga::model
