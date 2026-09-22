#include "nga/model/Transition.hpp"

#include "nga/model/Prune.hpp"
#include "nga/model/Slots.hpp"
#include "nga/model/Transform.hpp"

#include <algorithm>
#include <string>
#include <string_view>
#include <utility>

namespace nga::model
{

namespace
{

/// The Transition routine, compiled into the tool as the text it is, and
/// knowing no hardware: what it reads it reads through the driver's stream,
/// and what it switches it switches through the driver. What it does is the
/// contract in docs/spec/transition.md.
constexpr std::string_view ROUTINE_SOURCE =
    R"asm(; The Transition routine. Called by `jsr` from a `.transition`, with the
; statement's list right behind the call: the Phase to enter, and per Phase
; the code may be in, the unit and offset where the edge's Frame waits.
; nga.open and nga.read are the driver's roles, macros of the driver's
; Module expanded here; ngaShowBases is the Proc the tool generates beside
; the dispatcher, which reads from the Frame the base the entered Phase gives
; every Window and shows it, the stream's last; and ngaCurrentPhase is the
; Cell the tool generates. Everything else the edge has to say waits in the
; Frame, in storage. See docs/spec/transition.md.
;
; Its zero page is Temporaries: nothing of it is needed once the routine has
; jumped to the entered Phase's entry, and a `.transition` is a jump into the
; routine, so whatever the statement's Section had on the zero page is dead by
; then too. Two Temporaries never live at once share an address — see
; docs/decisions/0034-trace.md.

.export ngaTransition

ngaPtr .ztemp 2
ngaEntry .ztemp 2
ngaDst .ztemp 2
ngaValue .ztemp 2
ngaOffset .ztemp 2
ngaFramePos .ztemp 2
ngaWanted .ztemp 1
ngaCount .ztemp 1
ngaFrameUnit .ztemp 1
ngaUnit .ztemp 1

.proc ngaTransition
        pla
        sta ngaPtr
        pla
        sta ngaPtr+1            ; the return address: one below the statement's list
        ldy #1
        lda (ngaPtr),y
        sta ngaWanted           ; the Phase to enter
        iny
        lda (ngaPtr),y
        sta ngaCount            ; entries that follow
        lda ngaPtr
        clc
        adc #3
        sta ngaPtr
        bcc @scan
        inc ngaPtr+1
@scan
        lda ngaCount
        bne @check
        brk                     ; no entry for the current Phase: unreachable while the static rule holds
@check
        ldy #0
        lda (ngaPtr),y
        cmp ngaCurrentPhase
        beq @found
        lda ngaPtr              ; the next entry: four bytes on
        clc
        adc #4
        sta ngaPtr
        bcc @skipped
        inc ngaPtr+1
@skipped
        dec ngaCount
        jmp @scan
@found
        iny
        lda (ngaPtr),y
        sta ngaFrameUnit        ; the unit the Frame waits in
        iny
        lda (ngaPtr),y
        sta ngaFramePos
        iny
        lda (ngaPtr),y
        sta ngaFramePos+1       ; and where in it
        jsr ngaFrameOpen
        nga.read
        sta ngaEntry
        nga.read
        sta ngaEntry+1
        nga.read
        sta ngaCount            ; blocks that follow
        lda #3
        jsr ngaFrameSkip
@block
        lda ngaCount
        beq @cells
        jsr ngaFrameOpen        ; back to the Frame: a block's stream replaced it
        nga.read
        sta ngaUnit
        nga.read
        sta ngaOffset
        nga.read
        sta ngaOffset+1
        nga.read
        sta ngaDst
        nga.read
        sta ngaDst+1
        nga.read
        pha                     ; the decoder's number
        lda #6
        jsr ngaFrameSkip
        lda ngaUnit
        ldx ngaOffset
        ldy ngaOffset+1
        nga.open                ; the block's stream: its stored size, then its bytes
        pla
        ldx ngaDst
        ldy ngaDst+1
        jsr ngaTransform
        dec ngaCount
        jmp @block
@cells
        jsr ngaFrameOpen
        nga.read
        sta ngaCount            ; Cell writes that follow, read in one stream
@cell
        lda ngaCount
        beq @enter
        nga.read
        sta ngaDst
        nga.read
        sta ngaDst+1
        nga.read
        sta ngaValue
        nga.read
        sta ngaValue+1
        ldy #0
        lda ngaValue
        sta (ngaDst),y          ; a Cell stands outside the driver's window
        iny
        lda ngaValue+1
        sta (ngaDst),y
        dec ngaCount
        jmp @cell
@enter
        jsr ngaShowBases        ; the entered Phase's base in every Window, read from the Frame
        lda ngaWanted
        sta ngaCurrentPhase     ; the Phase the program is in from here on
        jmp (ngaEntry)
.endp

; The Frame's stream, from where the routine last left it.
.proc ngaFrameOpen
        lda ngaFrameUnit
        ldx ngaFramePos
        ldy ngaFramePos+1
        nga.open
        rts
.endp

; A bytes on in the Frame, which is where its stream is opened next.
.proc ngaFrameSkip
        clc
        adc ngaFramePos
        sta ngaFramePos
        bcc @done
        inc ngaFramePos+1
@done
        rts
.endp
)asm";

constexpr std::string_view CELL_NAME = "ngaCurrentPhase";

/// Phase numbers travel in a byte, and `$FF` ends a table.
constexpr std::size_t MOST_PHASES = 0xFF;

void addOnce( std::vector<ModuleIndex>& list, ModuleIndex value )
{
  if ( std::ranges::find( list, value ) == list.end() )
  {
    list.push_back( value );
  }
}

void putByte( std::span<std::uint8_t> into, std::size_t at, std::uint32_t value )
{
  if ( at < into.size() )
  {
    into[at] = static_cast<std::uint8_t>( value & 0xFF );
  }
}

void putWord( std::span<std::uint8_t> into, std::size_t at, std::uint32_t value )
{
  putByte( into, at, value );
  putByte( into, at + 1, value >> 8 );
}

/// The runtime address of a Label the whole program can see, or nothing.
std::optional<std::uint32_t>
addressOfExported( GlobalSymbols const& symbols, std::string_view name, Sizes const& sizes, Layout const& layout )
{
  std::optional<SymbolRef> const where = symbols.find( name );
  return where.has_value() ? addressOfLabel( symbols, *where, sizes, layout ) : std::nullopt;
}

} // namespace

std::optional<std::uint32_t>
addressOfLabel( GlobalSymbols const& symbols, SymbolRef where, Sizes const& sizes, Layout const& layout )
{
  Symbol const& symbol = symbols.at( where );
  if ( symbol.kind != SymbolKind::LABEL )
  {
    return std::nullopt;
  }
  LabelPosition const position = std::get<LabelPosition>( symbol.value );
  SectionRef const section{ .module = where.module, .section = position.section };
  if ( !layout.isPlaced( section ) || !sizes.isKnown( section ) )
  {
    return std::nullopt;
  }
  return layout.addressOf( section ) + sizes.offsetOf( section, position.chunk );
}

void addTransitionModules( Project& project, diag::SourceManager& sources, diag::DiagnosticSink& sink )
{
  PhaseGraph& graph = project.phases;
  bool const anyEdge = std::ranges::any_of( graph.phases, []( Phase const& phase ) { return !phase.then.empty(); } );
  if ( !anyEdge )
  {
    return;
  }

  if ( graph.phases.size() > MOST_PHASES - 1 )
  {
    sink.add( diag::diagnostic( diag::DiagnosticId::TOO_MANY_PHASES )
                  .arg( "count", static_cast<std::int64_t>( graph.phases.size() ) )
                  .sortedBy( "phases" ) );
    return;
  }

  auto const addModule = [&project]( ProjectModule module ) -> ModuleIndex
  {
    ModuleIndex const index{ static_cast<std::uint32_t>( project.modules.size() ) };
    project.modules.push_back( std::move( module ) );
    return index;
  };

  // The routine: the tool's own source, knowing no hardware. What it calls
  // is the driver's, under names the dispatcher defines at the end of
  // Assemble.
  diag::FileId const routineFile = sources.addFile( "<nga>/transition.asm", std::string{ ROUTINE_SOURCE } );
  ModuleIndex const routineModule = addModule( ProjectModule{ .name = "nga.transition",
                                                              .file = routineFile,
                                                              .residency = {},
                                                              .generated = Generated::NONE,
                                                              .outsideWindow = true } );
  project.transitionRoutine = routineModule;

  // The Cell: its pseudo-source is the one name it exports, so that the
  // Symbol's name is text the SourceManager owns like every other name.
  diag::FileId const cellFile = sources.addFile( "<nga>/cell", std::string{ CELL_NAME } );
  // Outside the Window, as every Cell is: the routine writes it with the
  // driver's stream open, which on a mapped driver is a Bank in.
  ModuleIndex const cellModule = addModule( ProjectModule{ .name = "nga.cell",
                                                           .file = cellFile,
                                                           .residency = {},
                                                           .generated = Generated::TRANSITION_CELL,
                                                           .outsideWindow = true } );
  project.transitionCell = cellModule;

  // Nothing declares Residency: the generator lists what it added in every
  // Phase's needs and lets the derivation say the rest.
  for ( Phase& phase : graph.phases )
  {
    addOnce( phase.needs, routineModule );
    addOnce( phase.needs, cellModule );
  }
  deriveResidency( project );
}

Module buildGeneratedModule( diag::SourceManager const& sources, ProjectModule const& entry )
{
  Module module{ entry.name, entry.file, entry.residency };
  diag::SourceSpan const span{ .begin = sources.locationOf( entry.file, 0 ), .length = 0 };

  Section section{ PlacementClass::ABSOLUTE, nullptr, nullptr, nullptr, false, false, SectionKind::PLAIN, span };
  section.appendChunk( TransitionCellContent{}, span, {} );
  SectionIndex const index = module.addSection( std::move( section ) );

  {
    // The routine reaches the Cell by name, so the Cell exports one: a Label at
    // its only Chunk, whose text is the pseudo-source this Module was
    // registered with.
    diag::SourceSpan const name{ .begin = sources.locationOf( entry.file, 0 ),
                                 .length = static_cast<std::uint32_t>( CELL_NAME.size() ) };
    module.symbols().add(
        Symbol{ .name = sources.textOf( name ),
                .kind = SymbolKind::LABEL,
                .definition = name,
                .exported = true,
                .value = LabelPosition{ .section = index, .chunk = ChunkIndex{ 0 }, .inner = std::nullopt } } );
  }
  return module;
}

std::vector<SectionRef>
loadSetOf( PhaseGraph const& graph, PhaseIndex from, PhaseIndex to, std::span<Module const> modules )
{
  std::vector<SectionRef> loaded;
  Phase const& left = graph.phases[from.value];
  for ( ModuleIndex const module : graph.phases[to.value].needs )
  {
    bool const kept = std::ranges::find( left.needs, module ) != left.needs.end();
    Module const& one = modules[module.value];
    for ( std::uint32_t index = 0; index < one.sections().size(); ++index )
    {
      // A Pane's Section is loaded once, by the Container into its Bank, and
      // no edge copies it — see docs/decisions/0054-panes.md.
      Section const& section = one.sections()[index];
      if ( section.emitsBytes() && !section.pane().has_value() && ( !kept || section.isMovable() ) )
      {
        loaded.push_back( SectionRef{ .module = module, .section = SectionIndex{ index } } );
      }
    }
  }
  // Project order, whatever order the needs were written in.
  std::ranges::sort( loaded,
                     []( SectionRef const& a, SectionRef const& b )
                     {
                       return a.module.value != b.module.value ? a.module.value < b.module.value
                                                               : a.section.value < b.section.value;
                     } );
  return loaded;
}

void resolveTransitions( diag::SourceManager const& sources,
                         Project const& project,
                         std::span<Module> modules,
                         diag::DiagnosticSink& sink )
{
  PhaseGraph const& graph = project.phases;

  // What a Frame can count: an edge loading more Sections than a byte holds
  // is refused here, where the edge is, whether or not a statement takes it.
  for ( std::uint32_t from = 0; from < graph.phases.size(); ++from )
  {
    for ( PhaseIndex const next : graph.phases[from].then )
    {
      std::vector<SectionRef> const loaded = loadSetOf( graph, PhaseIndex{ from }, next, modules );
      if ( loaded.size() > MOST_BLOCKS )
      {
        std::string const fromName = graph.phases[from].name.value_or( "(implicit)" );
        std::string const toName = graph.phases[next.value].name.value_or( "(implicit)" );
        std::string key = fromName;
        key += ' ';
        key += toName;
        sink.add( diag::diagnostic( diag::DiagnosticId::TRANSITION_LOADS_TOO_MUCH )
                      .arg( "from", fromName )
                      .arg( "to", toName )
                      .arg( "count", static_cast<std::int64_t>( loaded.size() ) )
                      .sortedBy( std::move( key ) ) );
      }
    }
  }

  // Which edges some `.transition` takes: `taken[from][to]`. An edge no
  // statement takes is a Phase that nothing can enter, and Prune will drop
  // it, so the author is told here, where the mirror finding is raised.
  std::vector<std::vector<bool>> taken( graph.phases.size(), std::vector<bool>( graph.phases.size(), false ) );

  for ( Module& module : modules )
  {
    for ( std::uint32_t sectionIndex = 0; sectionIndex < module.sections().size(); ++sectionIndex )
    {
      Section& section = module.sectionAt( SectionIndex{ sectionIndex } );
      for ( std::uint32_t chunkIndex = 0; chunkIndex < section.chunks().size(); ++chunkIndex )
      {
        Chunk& chunk = section.chunkAt( ChunkIndex{ chunkIndex } );

        auto* const transition = std::get_if<TransitionContent>( &chunk.content );
        if ( transition == nullptr )
        {
          continue;
        }

        std::string_view const name = sources.textOf( transition->name.span() );
        std::optional<PhaseIndex> target;
        for ( std::uint32_t phase = 0; phase < graph.phases.size(); ++phase )
        {
          if ( graph.phases[phase].name == name )
          {
            target = PhaseIndex{ phase };
            break;
          }
        }
        if ( !target.has_value() )
        {
          sink.add( diag::diagnostic( diag::DiagnosticId::TRANSITION_TO_UNKNOWN_PHASE )
                        .at( transition->name.location, transition->name.length )
                        .arg( "phase", std::string{ name } ) );
          continue;
        }

        // At run time the code does not know which Phase it is in, so every
        // Phase it is present in has to have the edge: that is what makes the
        // routine's failure path unreachable.
        bool complete = true;
        for ( std::uint32_t phase = 0; phase < graph.phases.size(); ++phase )
        {
          if ( !module.residency().includes( PhaseIndex{ phase } ) )
          {
            continue;
          }
          std::vector<PhaseIndex> const& then = graph.phases[phase].then;
          if ( std::ranges::find( then, *target ) == then.end() )
          {
            sink.add( diag::diagnostic( diag::DiagnosticId::TRANSITION_WITHOUT_EDGE )
                          .at( transition->name.location, transition->name.length )
                          .arg( "to", std::string{ name } )
                          .arg( "from", graph.phases[phase].name.value_or( "(implicit)" ) ) );
            complete = false;
          }
        }
        if ( complete )
        {
          transition->target = target;
          for ( std::uint32_t phase = 0; phase < graph.phases.size(); ++phase )
          {
            if ( module.residency().includes( PhaseIndex{ phase } ) )
            {
              taken[phase][target->value] = true;
            }
          }
        }
      }
    }
  }

  for ( std::uint32_t from = 0; from < graph.phases.size(); ++from )
  {
    for ( PhaseIndex const to : graph.phases[from].then )
    {
      if ( taken[from][to.value] )
      {
        continue;
      }
      std::string const fromName = graph.phases[from].name.value_or( "(implicit)" );
      std::string const toName = graph.phases[to.value].name.value_or( "(implicit)" );
      std::string key = fromName;
      key += ' ';
      key += toName;
      sink.add( diag::diagnostic( diag::DiagnosticId::EDGE_NOT_TAKEN )
                    .arg( "from", fromName )
                    .arg( "to", toName )
                    .sortedBy( std::move( key ) ) );
    }
  }
}

namespace
{

/// What the Modules a Phase needs define under its entry's name.
struct EntryLookup
{
  Symbol const* found = nullptr;
  ModuleIndex home;
  bool ambiguous = false;
};

EntryLookup lookupEntry( PhaseGraph const& graph, PhaseIndex phase, GlobalSymbols const& symbols )
{
  Phase const& entered = graph.phases[phase.value];
  std::string const name = entered.entry.value_or( "entry" );
  EntryLookup result;
  for ( ModuleIndex const module : entered.needs )
  {
    Symbol const* const candidate = symbols.moduleAt( module ).symbols().find( name );
    if ( candidate == nullptr )
    {
      continue;
    }
    if ( result.found != nullptr )
    {
      result.ambiguous = true;
      break;
    }
    result.found = candidate;
    result.home = module;
  }
  return result;
}

} // namespace

std::optional<SymbolRef> entrySymbolOf( PhaseGraph const& graph, PhaseIndex phase, GlobalSymbols const& symbols )
{
  EntryLookup const lookup = lookupEntry( graph, phase, symbols );
  if ( lookup.found == nullptr || lookup.ambiguous || lookup.found->kind != SymbolKind::LABEL )
  {
    return std::nullopt;
  }
  std::vector<Symbol> const& table = symbols.moduleAt( lookup.home ).symbols().symbols();
  return SymbolRef{ .module = lookup.home, .index = static_cast<std::uint32_t>( lookup.found - table.data() ) };
}

std::optional<std::uint32_t> entryAddressOf( Placed const& build, PhaseIndex phase, diag::DiagnosticSink& sink )
{
  GlobalSymbols const& symbols = build.symbols();
  Layout const& layout = build.layout();
  Sizes const& sizes = build.sizes();
  Phase const& entered = build.phases().phases[phase.value];
  std::string const name = entered.entry.value_or( "entry" );
  std::string const phaseName = entered.name.value_or( "(implicit)" );

  auto const [found, home, ambiguous] = lookupEntry( build.phases(), phase, symbols );

  if ( found == nullptr )
  {
    sink.add( diag::diagnostic( diag::DiagnosticId::ENTRY_NOT_DEFINED )
                  .arg( "phase", phaseName )
                  .arg( "name", name )
                  .sortedBy( phaseName ) );
    return std::nullopt;
  }
  if ( ambiguous )
  {
    sink.add( diag::diagnostic( diag::DiagnosticId::ENTRY_AMBIGUOUS )
                  .at( found->definition.begin, found->definition.length )
                  .arg( "phase", phaseName )
                  .arg( "name", name ) );
    return std::nullopt;
  }
  if ( found->kind != SymbolKind::LABEL )
  {
    sink.add( diag::diagnostic( diag::DiagnosticId::ENTRY_NOT_A_LABEL )
                  .at( found->definition.begin, found->definition.length )
                  .arg( "name", name )
                  .arg( "kind", std::string{ nameOf( found->kind ) } ) );
    return std::nullopt;
  }

  LabelPosition const position = std::get<LabelPosition>( found->value );
  SectionRef const where{ .module = home, .section = position.section };
  if ( !layout.isPlaced( where ) || !sizes.isKnown( where ) )
  {
    return std::nullopt;
  }
  // Where the Section stands in the Phase entered, which for a Movable one
  // is the only address that means anything here.
  return layout.addressIn( where, phase ) + sizes.offsetOf( where, position.chunk );
}

EntryAddresses::EntryAddresses( Placed const& build, diag::DiagnosticSink& sink )
    : mBuild( &build ), mSink( &sink ), mResolved( build.phases().phases.size() )
{
}

std::optional<std::uint32_t> EntryAddresses::of( PhaseIndex phase )
{
  std::optional<std::optional<std::uint32_t>>& slot = mResolved[phase.value];
  if ( !slot.has_value() )
  {
    slot = entryAddressOf( *mBuild, phase, *mSink );
  }
  return *slot;
}

bool isEnteredByAnEdge( PhaseGraph const& graph, PhaseIndex phase )
{
  return std::ranges::any_of(
      graph.phases, [phase]( Phase const& from ) { return std::ranges::find( from.then, phase ) != from.then.end(); } );
}

std::uint32_t sizeOfTransition( Residency const& residency )
{
  std::uint32_t phases = 0;
  for ( std::uint32_t phase = 0; phase < residency.phaseCount(); ++phase )
  {
    if ( residency.includes( PhaseIndex{ phase } ) )
    {
      ++phases;
    }
  }
  return TRANSITION_HEADER_SIZE + ( TRANSITION_ENTRY_SIZE * phases );
}

std::uint32_t sizeOfFrame( std::uint32_t payloads, std::uint32_t cellWrites, std::uint32_t windows )
{
  return FRAME_HEADER_SIZE + ( FRAME_BLOCK_SIZE * payloads ) + 1 + ( CELL_WRITE_SIZE * cellWrites ) + windows;
}

std::vector<WindowIndex> baseOrderOf( Project const& project )
{
  std::vector<WindowIndex> order;
  std::optional<WindowIndex> const stream = project.driver.has_value() ? project.driver->stream : std::nullopt;
  for ( std::uint32_t index = 0; index < project.target.windows.size(); ++index )
  {
    if ( stream != WindowIndex{ index } )
    {
      order.push_back( WindowIndex{ index } );
    }
  }
  if ( stream.has_value() )
  {
    order.push_back( *stream );
  }
  return order;
}

std::string nameOfFrame( PhaseGraph const& graph, PhaseIndex from, PhaseIndex to )
{
  return "frame " + graph.phases[from.value].name.value_or( "(implicit)" ) + " -> " +
         graph.phases[to.value].name.value_or( "(implicit)" );
}

void writeTransition( TransitionContent const& content,
                      ModuleIndex home,
                      Placed const& build,
                      Storage const& storage,
                      std::span<std::uint8_t> into )
{
  Residency const& residency = build.symbols().moduleAt( home ).residency();
  std::uint32_t const routine =
      addressOfExported( build.symbols(), TRANSITION_ROUTINE_NAME, build.sizes(), build.layout() ).value_or( 0 );
  putByte( into, 0, 0x20 ); // jsr
  putWord( into, 1, routine );
  putByte( into, 3, content.target.has_value() ? content.target->value : 0 );
  std::size_t at = TRANSITION_HEADER_SIZE;
  std::uint32_t entries = 0;

  // An entry per Phase the Section is present in, in Phase order: the Phase,
  // and where the edge's Frame waits. An edge that does not exist has been
  // reported, and its entry is zeros the routine never selects.
  for ( std::uint32_t phase = 0; phase < residency.phaseCount(); ++phase )
  {
    PhaseIndex const from{ phase };
    if ( !residency.includes( from ) )
    {
      continue;
    }
    std::optional<FrameIndex> const frame =
        content.target.has_value() ? storage.frameOf( from, *content.target ) : std::nullopt;
    bool const known = frame.has_value() && storage.isFramePlaced( *frame );
    StorageAddress const waits = known ? storage.frameAddressOf( *frame ) : StorageAddress{};
    putByte( into, at, phase );
    putByte( into, at + 1, known ? waits.bank.value : 0 );
    putWord( into, at + 2, known ? waits.offset : 0 );
    at += TRANSITION_ENTRY_SIZE;
    ++entries;
  }
  putByte( into, 4, entries );
}

std::vector<std::uint8_t>
frameBytesOf( FrameIndex frame, Placed const& build, Storage const& storage, EntryAddresses& entries )
{
  GlobalSymbols const& symbols = build.symbols();
  Layout const& layout = build.layout();
  std::span<Module const> const modules = build.modules();
  std::vector<SymbolRef> const slots = slotsOf( modules );

  std::vector<std::uint8_t> bytes( storage.frameSizeOf( frame ), 0 );
  std::span<std::uint8_t> const into{ bytes };
  std::size_t at = 0;
  PhaseIndex const from = storage.frameFrom( frame );
  PhaseIndex const to = storage.frameTo( frame );

  // What the edge copies: everything it may have to, less a Movable Section
  // the Phase left already holds where the entered one wants it. The Frame
  // was sized for all of them, and a shorter list leaves the rest unread.
  std::vector<SectionRef> copied;
  for ( SectionRef const payload : storage.framePayloadsOf( frame ) )
  {
    // Sized for the whole load set; what is listed is what Prune kept.
    if ( !build.reachable().includes( payload ) )
    {
      continue;
    }
    Section const& section = modules[payload.module.value].sectionAt( payload.section );
    bool const held = section.isMovable() && modules[payload.module.value].residency().includes( from ) &&
                      layout.isPlaced( payload ) &&
                      layout.addressIn( payload, from ) == layout.addressIn( payload, to );
    if ( !held )
    {
      copied.push_back( payload );
    }
  }

  putWord( into, at, entries.of( to ).value_or( 0 ) );
  putByte( into, at + 2, static_cast<std::uint32_t>( copied.size() ) );
  at += FRAME_HEADER_SIZE;

  for ( SectionRef const payload : copied )
  {
    bool const known = storage.isPlaced( payload ) && layout.isPlaced( payload );
    StorageAddress const waits = known ? storage.addressOf( payload ) : StorageAddress{};
    putByte( into, at, known ? waits.bank.value : 0 );
    putWord( into, at + 1, known ? waits.offset : 0 );
    // Where the decoder's output lands: the Section's address in the Phase
    // entered plus the start of its initialised extent, which is Storage's
    // answer because the Payload was made from that extent and not from the
    // Section. Per edge, since a Movable Section lands where the entered
    // Phase holds it; the stored size travels with the form, which is one.
    putWord( into, at + 3, known ? layout.addressIn( payload, to ) + storage.landingOf( payload ) : 0 );
    putByte( into, at + 5, known ? storage.transformOf( payload ) : TRANSFORM_COPY );
    at += FRAME_BLOCK_SIZE;
  }

  // The Cells: every Slot with an Implementation live in the Phase entered
  // gets its Cell rewritten to that Implementation's address there. A
  // `vector` Cell keeps its `jmp` and is written one byte in. The Frame was
  // sized for every Slot, and a shorter list leaves the rest unread.
  std::vector<std::pair<std::uint32_t, std::uint32_t>> writes;
  for ( SymbolRef const slot : slots )
  {
    auto const& declaration = std::get<SlotDeclaration>( symbols.at( slot ).value );
    std::optional<SymbolRef> const live = implementationIn( symbols, slot, to );
    if ( !declaration.cell.has_value() || !live.has_value() || !layout.isPlaced( *declaration.cell ) )
    {
      continue;
    }
    std::uint32_t const cell =
        layout.addressOf( *declaration.cell ) + ( declaration.binding == Binding::VECTOR ? 1 : 0 );
    std::uint32_t const value = addressOfImplementation( build, *live, to ).value_or( 0 );
    writes.emplace_back( cell, value );
  }
  putByte( into, at, static_cast<std::uint32_t>( writes.size() ) );
  at += 1;
  for ( auto const& [cell, value] : writes )
  {
    putWord( into, at, cell );
    putWord( into, at + 2, value );
    at += CELL_WRITE_SIZE;
  }

  // The bases the entered Phase gives every Window, in the order the routine
  // shows them; a Window with no base at all gets zero, which its driver is
  // never handed since the routine shows nothing for it.
  for ( WindowIndex const window : baseOrderOf( build.project() ) )
  {
    putByte( into, at, baseIn( build.project(), to, window ).value_or( 0 ) );
    at += 1;
  }
  return bytes;
}

void writeSlotCell( SlotCellContent const& content, Placed const& build, std::span<std::uint8_t> into )
{
  PhaseGraph const& graph = build.phases();
  SymbolRef const slot{ .module = content.module, .index = content.symbol };
  std::optional<SymbolRef> const live = implementationIn( build.symbols(), slot, graph.entry );
  std::uint32_t const value = live.has_value() ? addressOfImplementation( build, *live, graph.entry ).value_or( 0 ) : 0;
  if ( content.binding == Binding::VECTOR )
  {
    putByte( into, 0, 0x4C ); // jmp
    putWord( into, 1, value );
    return;
  }
  putWord( into, 0, value );
}

void writeCell( Placed const& build, std::span<std::uint8_t> into )
{
  putByte( into, 0, build.phases().entry.value );
}

} // namespace nga::model
