#include "nga/model/Facts.hpp"

#include "nga/model/Tokens.hpp"

#include "nga/Json.hpp"
#include "nga/Version.hpp"
#include "nga/diag/Renderer.hpp"
#include "nga/diag/SeverityPolicy.hpp"

#include <array>
#include <cstddef>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace nga::model
{

namespace
{

// The order is the order `--fact` lists them in and the order they are written
// in, which is cheapest first.
constexpr std::array<std::pair<std::string_view, FactSet>, 7> NAMES = {
  std::pair{ std::string_view{ "diagnostics" }, FactSet::DIAGNOSTICS },
  std::pair{ std::string_view{ "project" }, FactSet::PROJECT },
  std::pair{ std::string_view{ "map" }, FactSet::MAP },
  std::pair{ std::string_view{ "sources" }, FactSet::SOURCES },
  std::pair{ std::string_view{ "tokens" }, FactSet::TOKENS },
  std::pair{ std::string_view{ "symbols" }, FactSet::SYMBOLS },
  std::pair{ std::string_view{ "bytes" }, FactSet::BYTES },
};

/// One file the build read or wrote, which is what `tokens` and `sources` are
/// written per. A `.ngc` Module is two of these: the C it was written as, and
/// the assembler it compiled to.
struct BuildFile
{
  std::string module;
  std::string path;
  diag::FileId file;
  Grammar grammar = Grammar::ASSEMBLER;
  bool onDisk = false;
  std::string_view role;
};

std::vector<BuildFile> filesOf( diag::SourceManager const& sources, Project const& project )
{
  std::vector<BuildFile> found;
  std::set<std::uint32_t> seen;

  // One entry per file, however many Modules name it. A Module the tool
  // generates carries the FileId of the Module it was generated from -- the
  // Slot Cells carry the file a Slot was declared in -- and tokens belong to
  // the file rather than to whoever claims it.
  auto once = [&seen]( diag::FileId file ) { return seen.insert( file.value ).second; };

  // The Project file is no Module, and neither is anything it includes -- the
  // variant a Target comes from, above all. They are the files nothing else
  // would hand over, and the only ones anywhere written in that grammar.
  for ( std::uint32_t i = 0; i < sources.fileCount(); ++i )
  {
    diag::FileId const file{ i };
    std::string_view const path = sources.pathOf( file );
    if ( grammarOf( path ) == Grammar::PROJECT && once( file ) )
    {
      found.push_back( { .module = {},
                         .path = std::string{ path },
                         .file = file,
                         .grammar = Grammar::PROJECT,
                         .onDisk = true,
                         .role = "project" } );
    }
  }

  for ( ProjectModule const& entry : project.modules )
  {
    // A Module the tool wrote the text of has no path and is on no disk, and
    // that is exactly what `sources` exists to hand over.
    bool const written = entry.fromGenerator || entry.generated != Generated::NONE || entry.path.empty();
    // The path the tool opened, not the path the entry wrote: `symbols` names
    // a file that way too, and two sets naming one file differently is a
    // consumer's bug waiting to happen.
    if ( !once( entry.file ) )
    {
      continue;
    }
    found.push_back( { .module = entry.name,
                       .path = std::string{ sources.pathOf( entry.file ) },
                       .file = entry.file,
                       .grammar = grammarOf( entry.path.empty() ? sources.pathOf( entry.file ) : entry.path ),
                       .onDisk = !written,
                       .role = "source" } );
    if ( entry.compiledText.has_value() )
    {
      found.push_back( { .module = entry.name,
                         .path = std::string{ sources.pathOf( *entry.compiledText ) },
                         .file = *entry.compiledText,
                         .grammar = Grammar::ASSEMBLER,
                         .onDisk = false,
                         .role = "compiled" } );
    }
  }
  return found;
}

std::string_view grammarName( Grammar grammar )
{
  switch ( grammar )
  {
  case Grammar::PROJECT:
    return "project";
  case Grammar::C:
    return "c";
  case Grammar::ASSEMBLER:
    return "assembler";
  }
  return "assembler";
}

std::string_view kindName( SectionKind kind )
{
  switch ( kind )
  {
  case SectionKind::PLAIN:
    return "section";
  case SectionKind::PROC:
    return "proc";
  case SectionKind::TEMPORARY:
    return "temporary";
  }
  return "section";
}

/// How a Module's text reached the tool, which is the one thing about a Module
/// that a consumer cannot work out from its path: a Generator's text and a
/// compiled `.ngc` exist nowhere on disk.
std::string_view originName( ProjectModule const& entry )
{
  if ( entry.fromGenerator )
  {
    return "generator";
  }
  if ( entry.compiledText.has_value() )
  {
    return "compiled";
  }
  // A Module the tool wrote the text of: the Cell and the named Regions, which
  // are `Generated`, and the runtime, the boot record and the Transition
  // routine, which are ordinary `.asm` the tool assembles from text it built.
  // Neither is on disk, so neither has a path.
  if ( entry.generated != Generated::NONE || entry.path.empty() )
  {
    return "tool";
  }
  return "file";
}

void appendField( std::string& out, std::string_view name, std::string_view value )
{
  json::appendString( out, name );
  out.append( ": " );
  json::appendString( out, value );
}

void appendNumber( std::string& out, std::string_view name, std::uint64_t value )
{
  json::appendString( out, name );
  out.append( ": " ).append( std::to_string( value ) );
}

void appendPhase( std::string& out, std::string_view name, std::optional<PhaseIndex> phase )
{
  json::appendString( out, name );
  out.append( ": " );
  out.append( phase.has_value() ? std::to_string( phase->value ) : "null" );
}

void appendNames( std::string& out, std::vector<std::string> const& names )
{
  out.push_back( '[' );
  for ( std::size_t i = 0; i < names.size(); ++i )
  {
    json::appendString( out, names[i] );
    if ( i + 1 < names.size() )
    {
      out.append( ", " );
    }
  }
  out.push_back( ']' );
}

void appendProject( std::string& out, Facts const& facts )
{
  Project const& project = *facts.project;

  out.append( "  \"project\": {\n    " );
  appendField( out, "path", facts.projectPath );
  out.append( ",\n    " );
  appendField( out, "container", nameOf( project.container ) );
  out.append( ",\n    " );
  appendField( out, "optimize", nameOf( project.intent ) );
  out.append( ",\n    " );
  appendField( out, "cpu", nameOf( project.target.cpu ) );
  out.append( ",\n    " );
  appendField( out, "library", project.library );

  out.append( ",\n    \"groups\": [" );
  for ( std::size_t i = 0; i < project.groups.size(); ++i )
  {
    out.append( i == 0 ? "\n      { " : ",\n      { " );
    appendField( out, "name", project.groups[i].name );
    out.append( ", \"members\": " );
    appendNames( out, project.groups[i].members );
    out.append( " }" );
  }
  out.append( project.groups.empty() ? "]" : "\n    ]" );

  out.append( ",\n    \"phases\": [\n" );
  for ( std::size_t i = 0; i < project.phases.phases.size(); ++i )
  {
    Phase const& phase = project.phases.phases[i];
    out.append( "      { " );
    appendNumber( out, "index", i );
    out.append( ", " );
    appendField( out, "name", phase.name.value_or( "" ) );
    out.append( ", \"entry\": " );
    out.append( i == project.phases.entry.value ? "true" : "false" );
    out.append( ", \"then\": [" );
    for ( std::size_t j = 0; j < phase.then.size(); ++j )
    {
      out.append( std::to_string( phase.then[j].value ) );
      if ( j + 1 < phase.then.size() )
      {
        out.append( ", " );
      }
    }
    out.append( "] }" );
    out.append( i + 1 < project.phases.phases.size() ? ",\n" : "\n" );
  }
  out.append( "    ],\n" );

  out.append( "    \"modules\": [\n" );
  for ( std::size_t i = 0; i < project.modules.size(); ++i )
  {
    ProjectModule const& entry = project.modules[i];
    out.append( "      { " );
    appendField( out, "name", entry.name );
    out.append( ", " );
    appendField( out, "path", entry.path );
    out.append( ", " );
    appendField( out, "origin", originName( entry ) );
    out.append( ", \"phases\": [" );
    bool first = true;
    for ( std::size_t p = 0; p < project.phases.phases.size(); ++p )
    {
      if ( p >= entry.residency.phaseCount() ||
           !entry.residency.includes( PhaseIndex{ static_cast<std::uint32_t>( p ) } ) )
      {
        continue;
      }
      if ( !std::exchange( first, false ) )
      {
        out.append( ", " );
      }
      out.append( std::to_string( p ) );
    }
    out.append( "] }" );
    out.append( i + 1 < project.modules.size() ? ",\n" : "\n" );
  }
  out.append( "    ],\n" );

  out.append( "    \"regions\": [\n" );
  for ( std::size_t i = 0; i < project.target.regions.size(); ++i )
  {
    Region const& region = project.target.regions[i];
    out.append( "      { " );
    appendField( out, "name", region.name );
    out.append( ", " );
    appendNumber( out, "begin", region.range.begin );
    out.append( ", " );
    appendNumber( out, "end", region.range.end );
    out.append( ", " );
    appendField( out, "property", nameOf( region.property ) );
    out.append( " }" );
    out.append( i + 1 < project.target.regions.size() ? ",\n" : "\n" );
  }
  out.append( "    ],\n" );

  Target const& target = project.target;
  out.append( "    \"unitSets\": [" );
  for ( std::size_t i = 0; i < target.unitSets.size(); ++i )
  {
    UnitSet const& set = target.unitSets[i];
    out.append( i == 0 ? "\n      { " : ",\n      { " );
    appendField( out, "name", set.name );
    out.append( ", " );
    appendNumber( out, "count", set.count );
    if ( set.linear.has_value() )
    {
      out.append( ", \"linear\": [" )
          .append( std::to_string( set.linear->begin ) )
          .append( ", " )
          .append( std::to_string( set.linear->end ) )
          .append( "]" );
    }
    out.append( " }" );
  }
  out.append( target.unitSets.empty() ? "],\n" : "\n    ],\n" );

  // A state is named in a byte by its index, which is what a Pane's
  // `paneState` is, so every entry says where it starts: a unit set is one
  // state per Bank from `index`.
  out.append( "    \"windows\": [" );
  for ( std::size_t i = 0; i < target.windows.size(); ++i )
  {
    Window const& window = target.windows[i];
    out.append( i == 0 ? "\n      { " : ",\n      { " );
    appendField( out, "name", window.name );
    out.append( ", \"ranges\": [" );
    for ( std::size_t r = 0; r < window.ranges.size(); ++r )
    {
      out.append( "[" )
          .append( std::to_string( window.ranges[r].begin ) )
          .append( ", " )
          .append( std::to_string( window.ranges[r].end ) )
          .append( r + 1 < window.ranges.size() ? "], " : "]" );
    }
    out.append( "], \"states\": [" );
    std::uint64_t index = 0;
    for ( std::size_t s = 0; s < window.states.size(); ++s )
    {
      WindowState const& state = window.states[s];
      out.append( s == 0 ? "{ " : ", { " );
      appendNumber( out, "index", index );
      out.append( ", " );
      if ( state.units.has_value() )
      {
        UnitSet const& set = target.unitSets[state.units->value];
        appendField( out, "units", set.name );
        out.append( ", " );
        appendNumber( out, "count", set.count );
        index += set.count;
      }
      else
      {
        appendField( out, "name", state.name );
        ++index;
      }
      out.append( " }" );
    }
    out.append( "] }" );
  }
  out.append( target.windows.empty() ? "],\n" : "\n    ],\n" );

  out.append( "    \"panes\": [" );
  for ( std::size_t i = 0; i < target.panes.size(); ++i )
  {
    Pane const& pane = target.panes[i];
    out.append( i == 0 ? "\n      { " : ",\n      { " );
    appendField( out, "name", pane.name );
    out.append( ", " );
    appendField( out, "window", target.windows[pane.window.value].name );
    if ( pane.state.has_value() )
    {
      out.append( ", " );
      appendNumber( out, "state", *pane.state );
    }
    if ( pane.count > 1 )
    {
      out.append( ", " );
      appendNumber( out, "count", pane.count );
    }
    out.append( " }" );
  }
  out.append( target.panes.empty() ? "]" : "\n    ]" );

  // Absent where there is no storage: a field that would say nothing is left
  // out. `units` names the set where storage is one, which is what finds the
  // Window that shows a unit and the linear address it starts at.
  if ( target.unitCount > 0 )
  {
    out.append( ",\n    \"storage\": { " );
    if ( target.storageUnits.has_value() )
    {
      appendField( out, "units", target.unitSets[target.storageUnits->value].name );
      out.append( ", " );
    }
    appendNumber( out, "count", target.unitCount );
    out.append( ", " );
    appendNumber( out, "size", target.unitSize );
    out.append( " }" );
  }
  out.append( "\n  }" );
}

void appendSite( std::string& out, diag::SourceManager const& sources, SymbolSite const& site )
{
  out.append( "{ " );
  appendField( out, "path", site.length == 0 && site.offset == 0 ? std::string_view{} : sources.pathOf( site.file ) );
  out.append( ", " );
  appendNumber( out, "offset", site.offset );
  out.append( ", " );
  appendNumber( out, "length", site.length );
  out.append( " }" );
}

void appendMap( std::string& out, diag::SourceManager const& sources, MemoryMap const& map )
{
  out.append( "  \"map\": {\n    " );
  appendField( out, "unit", map.unitWord );
  out.append( ",\n    " );
  appendNumber( out, "unitSize", map.bankSize );
  out.append( ",\n    " );
  // One number and not one per Phase: nothing loads ROM, so what stands there
  // stands there for the whole run.
  appendNumber( out, "romUsed", map.romUsed );
  out.append( ",\n    " );
  appendNumber( out, "romSize", map.pools.readOnlySize() );

  // What the solver allocates from, half-open as every range here is: what a
  // page of the map shades behind the Sections.
  out.append( ",\n    \"pools\": { " );
  auto const ranges = [&out]( std::string_view name, std::vector<AddressRange> const& pool, bool last )
  {
    json::appendString( out, name );
    out.append( ": [" );
    for ( std::size_t i = 0; i < pool.size(); ++i )
    {
      out.append( "[" )
          .append( std::to_string( pool[i].begin ) )
          .append( ", " )
          .append( std::to_string( pool[i].end ) );
      out.append( i + 1 < pool.size() ? "], " : "]" );
    }
    out.append( last ? "]" : "], " );
  };
  ranges( "zeroPage", map.pools.zeroPage, false );
  ranges( "general", map.pools.general, false );
  ranges( "readOnly", map.pools.readOnly, true );
  out.append( " }" );

  out.append( ",\n    \"phases\": [\n" );
  for ( std::size_t i = 0; i < map.phases.size(); ++i )
  {
    MapPhase const& phase = map.phases[i];
    out.append( "      { " );
    appendField( out, "name", phase.name );
    out.append( ", " );
    appendNumber( out, "zeroPageUsed", phase.zeroPageUsed );
    out.append( ", " );
    appendNumber( out, "memoryUsed", phase.generalUsed );
    out.append( " }" );
    out.append( i + 1 < map.phases.size() ? ",\n" : "\n" );
  }
  out.append( "    ],\n" );

  out.append( "    \"sections\": [\n" );
  for ( std::size_t i = 0; i < map.entries.size(); ++i )
  {
    MapEntry const& entry = map.entries[i];
    out.append( "      { " );
    appendField( out, "name", entry.name );
    out.append( ", " );
    appendField( out, "module", entry.module );
    out.append( ", " );
    appendField( out, "kind", kindName( entry.kind ) );
    out.append( ", " );
    appendField( out, "placement", nameOf( entry.placement ) );
    out.append( ", " );
    appendNumber( out, "begin", entry.begin );
    out.append( ", " );
    // Half-open in the model, as everywhere else it is written down; the text
    // map is the one place both ends are inclusive, because that is how a
    // reader expects to see a range of addresses.
    appendNumber( out, "end", entry.end );
    out.append( ", \"initialised\": [" )
        .append( std::to_string( entry.initialisedBegin ) )
        .append( ", " )
        .append( std::to_string( entry.initialisedEnd ) )
        .append( "], " );
    appendPhase( out, "firstPhase", entry.firstPhase );
    out.append( ", " );
    appendPhase( out, "lastPhase", entry.lastPhase );
    out.append( ", \"root\": " ).append( entry.root ? "true" : "false" );
    if ( entry.kept )
    {
      out.append( ", \"kept\": true" );
    }
    if ( entry.hidden )
    {
      out.append( ", \"hidden\": true" );
    }

    // What the text asked for, beside where the solver put it.
    if ( entry.pinned )
    {
      out.append( ", \"pinned\": true" );
    }
    if ( entry.alignment.has_value() )
    {
      out.append( ", " );
      appendNumber( out, "align", *entry.alignment );
    }
    if ( entry.boundary.has_value() )
    {
      out.append( ", " );
      appendNumber( out, "within", *entry.boundary );
    }
    if ( !entry.then.empty() )
    {
      out.append( ", " );
      appendField( out, "then", entry.then );
    }
    if ( !entry.regions.empty() )
    {
      out.append( ", \"in\": " );
      appendNames( out, entry.regions );
    }
    out.append( ", \"shares\": " );
    appendNames( out, entry.shares );

    if ( !entry.pane.empty() )
    {
      out.append( ", " );
      appendField( out, "pane", entry.pane );
      out.append( ", " );
      appendNumber( out, "paneState", entry.paneState );
    }
    if ( !entry.units.empty() )
    {
      out.append( ", " );
      appendField( out, "units", entry.units );
    }

    if ( !entry.waits.empty() )
    {
      out.append( ", \"waits\": { " );
      appendField( out, "as", entry.transform );
      out.append( ", " );
      appendPhase( out, "liveFirst", entry.liveFirst );
      out.append( ", " );
      appendPhase( out, "liveLast", entry.liveLast );
      // One piece unless no unit of storage held the whole of it: they are
      // placed apart, so a single unit and offset would name a run of the
      // medium that is not all the Payload's.
      out.append( ", \"pieces\": [ " );
      for ( std::size_t piece = 0; piece < entry.waits.size(); ++piece )
      {
        out.append( "{ " );
        appendNumber( out, "unit", entry.waits[piece].at.bank.value );
        out.append( ", " );
        appendNumber( out, "offset", entry.waits[piece].at.offset );
        out.append( ", " );
        appendNumber( out, "size", entry.waits[piece].size );
        out.append( piece + 1 < entry.waits.size() ? " }, " : " }" );
      }
      out.append( " ] }" );
    }

    out.append( " }" );
    out.append( i + 1 < map.entries.size() ? ",\n" : "\n" );
  }
  out.append( "    ],\n" );

  // What Prune dropped stands nowhere, so it has no address to be ordered
  // by; what a reader needs of it is where it is written.
  out.append( "    \"pruned\": [\n" );
  for ( std::size_t i = 0; i < map.pruned.size(); ++i )
  {
    MapPruned const& pruned = map.pruned[i];
    out.append( "      { " );
    appendField( out, "name", pruned.name );
    out.append( ", " );
    appendField( out, "module", pruned.module );
    out.append( ", " );
    appendField( out, "kind", kindName( pruned.kind ) );
    out.append( ", \"text\": [" );
    for ( std::size_t piece = 0; piece < pruned.text.size(); ++piece )
    {
      appendSite( out, sources, pruned.text[piece] );
      if ( piece + 1 < pruned.text.size() )
      {
        out.append( ", " );
      }
    }
    out.append( "] }" );
    out.append( i + 1 < map.pruned.size() ? ",\n" : "\n" );
  }
  out.append( "    ],\n" );

  out.append( "    \"frames\": [\n" );
  for ( std::size_t i = 0; i < map.frames.size(); ++i )
  {
    MapFrame const& frame = map.frames[i];
    out.append( "      { " );
    appendField( out, "name", frame.name );
    out.append( ", " );
    appendNumber( out, "unit", frame.waits.bank.value );
    out.append( ", " );
    appendNumber( out, "offset", frame.waits.offset );
    out.append( ", " );
    appendNumber( out, "size", frame.size );
    out.append( ", " );
    appendPhase( out, "liveFirst", frame.liveFirst );
    out.append( ", " );
    appendPhase( out, "liveLast", frame.liveLast );
    out.append( " }" );
    out.append( i + 1 < map.frames.size() ? ",\n" : "\n" );
  }
  out.append( "    ],\n" );

  out.append( "    \"units\": [\n" );
  for ( std::size_t i = 0; i < map.banks.size(); ++i )
  {
    out.append( "      { " );
    appendNumber( out, "index", i );
    out.append( ", " );
    appendNumber( out, "used", map.banks[i].used );
    out.append( " }" );
    out.append( i + 1 < map.banks.size() ? ",\n" : "\n" );
  }
  out.append( "    ]\n  }" );
}

void appendSources( std::string& out, diag::SourceManager const& sources, std::vector<BuildFile> const& files )
{
  out.append( "  \"sources\": [\n" );
  bool first = true;
  for ( BuildFile const& held : files )
  {
    if ( held.onDisk )
    {
      // The consumer has the path and can open it. Copying it in would double
      // the document for nothing.
      continue;
    }
    if ( !std::exchange( first, false ) )
    {
      out.append( ",\n" );
    }
    out.append( "    { " );
    appendField( out, "module", held.module );
    out.append( ", " );
    // What every other set names the file by: without it a use in `symbols`
    // that points into the runtime has nothing here to be joined to.
    appendField( out, "path", held.path );
    out.append( ", " );
    appendField( out, "role", held.role );
    out.append( ", " );
    appendField( out, "grammar", grammarName( held.grammar ) );
    out.append( ", " );
    appendField( out, "text", sources.contentsOf( held.file ) );
    out.append( " }" );
  }
  out.append( first ? "  ]" : "\n  ]" );
}

void appendTokens( std::string& out,
                   diag::SourceManager const& sources,
                   diag::DiagnosticSink& quiet,
                   std::vector<BuildFile> const& files )
{
  out.append( "  \"tokens\": [\n" );
  for ( std::size_t i = 0; i < files.size(); ++i )
  {
    BuildFile const& held = files[i];
    out.append( "    { " );
    appendField( out, "module", held.module );
    out.append( ", " );
    appendField( out, "path", held.path );
    out.append( ", " );
    appendField( out, "role", held.role );
    out.append( ", " );
    appendField( out, "grammar", grammarName( held.grammar ) );
    out.append( ",\n      \"spans\": [" );

    std::vector<ClassifiedToken> const spans = classify( sources, held.file, held.grammar, quiet );
    for ( std::size_t s = 0; s < spans.size(); ++s )
    {
      ClassifiedToken const& span = spans[s];
      out.append( "[" ).append( std::to_string( span.offset ) );
      out.append( "," ).append( std::to_string( span.length ) ).append( "," );
      json::appendString( out, nameOf( span.classification ) );
      out.append( s + 1 < spans.size() ? "], " : "]" );
    }
    out.append( "] }" );
    out.append( i + 1 < files.size() ? ",\n" : "\n" );
  }
  out.append( "  ]" );
}

void appendSymbols( std::string& out, diag::SourceManager const& sources, std::vector<SymbolFacts> const& symbols )
{
  out.append( "  \"symbols\": [\n" );
  for ( std::size_t i = 0; i < symbols.size(); ++i )
  {
    SymbolFacts const& symbol = symbols[i];
    out.append( "    { " );
    appendField( out, "name", symbol.name );
    out.append( ", " );
    appendField( out, "module", symbol.module );
    out.append( ", " );
    appendField( out, "kind", symbol.kind );
    if ( symbol.exported )
    {
      out.append( ", \"exported\": true" );
    }
    if ( !symbol.section.empty() )
    {
      out.append( ", " );
      appendField( out, "section", symbol.section );
    }
    if ( symbol.address.has_value() )
    {
      out.append( ", \"address\": " ).append( std::to_string( *symbol.address ) );
    }
    if ( symbol.value.has_value() )
    {
      out.append( ", \"value\": " ).append( std::to_string( *symbol.value ) );
    }
    if ( !symbol.pane.empty() )
    {
      out.append( ", " );
      appendField( out, "pane", symbol.pane );
      out.append( ", " );
      appendNumber( out, "paneState", symbol.paneState );
    }
    if ( !symbol.units.empty() )
    {
      out.append( ", " );
      appendField( out, "units", symbol.units );
    }
    out.append( ", \"definition\": " );
    appendSite( out, sources, symbol.definition );
    out.append( ", \"uses\": [" );
    for ( std::size_t u = 0; u < symbol.uses.size(); ++u )
    {
      appendSite( out, sources, symbol.uses[u] );
      if ( u + 1 < symbol.uses.size() )
      {
        out.append( ", " );
      }
    }
    out.append( "] }" );
    out.append( i + 1 < symbols.size() ? ",\n" : "\n" );
  }
  out.append( "  ]" );
}

void appendRun( std::string& out, diag::SourceManager const& sources, ByteRun const& run, std::string_view indent )
{
  out.append( indent ).append( "{ " );
  appendNumber( out, "at", run.at );
  out.append( ", " );
  appendNumber( out, "length", run.length );
  out.append( ", " );
  appendNumber( out, "address", run.address );
  out.append( ", " );
  appendField( out, "what", run.what );
  if ( !run.section.empty() )
  {
    out.append( ", " );
    appendField( out, "section", run.section );
    out.append( ", " );
    appendField( out, "module", run.module );
    out.append( ", \"chunk\": " ).append( std::to_string( run.chunk ) );
  }
  if ( run.source.length != 0 )
  {
    out.append( ", \"source\": { " );
    appendField( out, "path", sources.pathOf( run.source.file ) );
    out.append( ", " );
    appendNumber( out, "offset", run.source.offset );
    out.append( ", " );
    appendNumber( out, "length", run.source.length );
    out.append( " }" );
  }
  if ( !run.data.empty() )
  {
    // Two hex digits a byte, which a reader shows as the listing does.
    static constexpr std::string_view DIGITS = "0123456789abcdef";
    std::string data;
    data.reserve( run.data.size() * 2 );
    for ( std::uint8_t const byte : run.data )
    {
      data.push_back( DIGITS[byte >> 4U] );
      data.push_back( DIGITS[byte & 0x0FU] );
    }
    out.append( ", " );
    appendField( out, "data", data );
  }
  out.append( " }" );
}

void appendBytes( std::string& out, diag::SourceManager const& sources, ContainerFacts const& facts )
{
  out.append( "  \"bytes\": {\n    " );
  appendField( out, "container", facts.container );
  out.append( ",\n    " );
  appendNumber( out, "size", facts.size );
  out.append( ",\n    " );
  appendNumber( out, "unaccounted", facts.unaccounted );

  out.append( ",\n    \"header\": [\n" );
  for ( std::size_t i = 0; i < facts.header.size(); ++i )
  {
    appendRun( out, sources, facts.header[i], "      " );
    out.append( i + 1 < facts.header.size() ? ",\n" : "\n" );
  }
  out.append( "    ],\n    \"segments\": [\n" );

  for ( std::size_t s = 0; s < facts.segments.size(); ++s )
  {
    ContainerSegment const& segment = facts.segments[s];
    out.append( "      { " );
    appendNumber( out, "at", segment.at );
    out.append( ", " );
    appendNumber( out, "start", segment.start );
    out.append( ", " );
    appendNumber( out, "end", segment.end );
    out.append( ",\n        \"runs\": [\n" );
    for ( std::size_t r = 0; r < segment.runs.size(); ++r )
    {
      appendRun( out, sources, segment.runs[r], "          " );
      out.append( r + 1 < segment.runs.size() ? ",\n" : "\n" );
    }
    out.append( "        ] }" );
    out.append( s + 1 < facts.segments.size() ? ",\n" : "\n" );
  }
  out.append( "    ]\n  }" );
}

} // namespace

std::optional<FactSet> factSetFor( std::string_view name )
{
  for ( auto const& [spelling, set] : NAMES )
  {
    if ( spelling == name )
    {
      return set;
    }
  }
  return std::nullopt;
}

std::set<FactSet> everyFactSet()
{
  std::set<FactSet> all;
  for ( auto const& [name, set] : NAMES )
  {
    all.insert( set );
  }
  return all;
}

std::string factSetNames()
{
  std::string out;
  for ( std::size_t i = 0; i < NAMES.size(); ++i )
  {
    out.append( NAMES[i].first );
    if ( i + 1 < NAMES.size() )
    {
      out.append( ", " );
    }
  }
  return out;
}

std::string renderFactsJson( diag::SourceManager const& sources,
                             diag::DiagnosticSink const& sink,
                             Facts const& facts,
                             std::set<FactSet> const& wanted )
{
  std::string out;
  out.append( "{\n  \"schema\": 2,\n  " );
  appendField( out, "nga", versionString() );

  // Always, whatever was asked for: a document of a build that failed and
  // says nothing about why would be worse than no document -- 0213.
  out.append( ",\n" );
  diag::appendJsonFindings( out, sources, sink, "  " );

  if ( wanted.contains( FactSet::PROJECT ) && facts.project != nullptr )
  {
    out.append( ",\n" );
    appendProject( out, facts );
  }

  if ( wanted.contains( FactSet::MAP ) && facts.map.has_value() )
  {
    out.append( ",\n" );
    appendMap( out, sources, *facts.map );
  }

  if ( wanted.contains( FactSet::SYMBOLS ) && facts.symbols.has_value() )
  {
    out.append( ",\n" );
    appendSymbols( out, sources, *facts.symbols );
  }

  if ( wanted.contains( FactSet::BYTES ) && facts.bytes.has_value() )
  {
    out.append( ",\n" );
    appendBytes( out, sources, *facts.bytes );
  }

  // Lexing a file a second time raises what it raised the first time, and the
  // build has already reported it. The findings go nowhere.
  if ( facts.project != nullptr && ( wanted.contains( FactSet::SOURCES ) || wanted.contains( FactSet::TOKENS ) ) )
  {
    std::vector<BuildFile> const files = filesOf( sources, *facts.project );
    diag::SeverityPolicy policy;
    diag::DiagnosticSink quiet{ policy };

    if ( wanted.contains( FactSet::SOURCES ) )
    {
      out.append( ",\n" );
      appendSources( out, sources, files );
    }
    if ( wanted.contains( FactSet::TOKENS ) )
    {
      out.append( ",\n" );
      appendTokens( out, sources, quiet, files );
    }
  }

  out.append( "\n}\n" );
  return out;
}

} // namespace nga::model
