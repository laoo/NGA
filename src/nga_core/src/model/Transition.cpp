#include "nga/model/Transition.hpp"

#include "nga/model/Assemble.hpp"
#include "nga/model/Prune.hpp"
#include "nga/model/Segments.hpp"
#include "nga/model/Slots.hpp"
#include "nga/model/Storage.hpp"
#include "nga/model/Transform.hpp"

#include <spdlog/fmt/fmt.h>

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
/// The cold start, for a Container that has no loader: the Frame of the edge
/// into the entry Phase from nowhere, and the Proc the run vector points at,
/// which hands that Frame to the routine's tail. Where the Frame waits is three
/// bytes the Container patches, as it patches the run vector itself — nothing
/// knows them until every Payload has been placed. See docs/spec/car.md and
/// docs/decisions/0216-a-car-names-its-format-and-the-cold-start-is-an-edge.md.
constexpr std::string_view COLD_START_SOURCE =
    R"asm(
; Where the cold start's Frame waits: a unit and an offset in it, which the
; Container writes here once PlaceStorage has put it somewhere. `readonly`
; because nothing writes it and it belongs in the ROM a cartridge is.
.export ngaColdFrame
.section absolute, readonly
ngaColdFrame
        .byte 0                 ; the unit
.if ngaUnitBytes > 1
        .byte 0
.endif
        .word 0                 ; and where in it
.ends

; What a cartridge's run vector points at, and what the OS therefore jumps to:
; the entry Phase is entered as any Phase is entered, by the routine, from a
; Frame. `root`, since the hardware reaches it and no Chunk of the program does,
; and exported because the Container writes its address into the run vector.
.export ngaStart
.proc ngaStart, root
        lda #{}
        sta ngaWanted
        lda ngaColdFrame
        sta ngaFrameAt+2
.if ngaUnitBytes > 1
        lda ngaColdFrame+1
        sta ngaFrameAt+3
.endif
        lda ngaColdFrame+ngaUnitBytes
        sta ngaFrameAt
        lda ngaColdFrame+ngaUnitBytes+1
        sta ngaFrameAt+1
        jmp ngaEnter
.endp
)asm";

constexpr std::string_view ROUTINE_SOURCE =
    R"asm(; The Transition routine. Called by `jsr` from a `.transition`, with the
; statement's list right behind the call: the Phase to enter, and per Phase
; the code may be in, the unit and offset where the edge's Frame waits.
; nga.open and nga.read are the driver's roles, macros of the driver's
; Module expanded here, and nga.unit and nga.offset the cells its stream
; stands in, which open reads the position from; and ngaCurrentPhase is the
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
ngaFrameAt .ztemp 2 + ngaUnitBytes ; where the Frame's stream stands: the offset, then the unit
ngaWanted .ztemp 1
ngaCount .ztemp 1
ngaUnit .ztemp ngaUnitBytes

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
        lda ngaPtr              ; the next entry
        clc
        adc #ngaEntrySize
        sta ngaPtr
        bcc @skipped
        inc ngaPtr+1
@skipped
        dec ngaCount
        jmp @scan
@found
        iny
        lda (ngaPtr),y
        sta ngaFrameAt+2        ; the unit the Frame waits in
.if ngaUnitBytes > 1
        iny
        lda (ngaPtr),y
        sta ngaFrameAt+3
.endif
        iny
        lda (ngaPtr),y
        sta ngaFrameAt
        iny
        lda (ngaPtr),y
        sta ngaFrameAt+1        ; and where in it
.endp then ngaEnter

; The rest of it, and the way in for a Container that has no current Phase to
; look one up by: ngaWanted holds the Phase to enter and ngaFrameAt where
; its Frame waits. The cold start comes here — see
; docs/decisions/0216-a-car-names-its-format-and-the-cold-start-is-an-edge.md.
.export ngaEnter
.proc ngaEnter
        jsr ngaFrameOpen
.if ngaFramesHeld
        nga.read
        sta ngaPtr
        nga.read
        sta ngaPtr+1            ; where the descriptors are to be held, or zero
.endif
        nga.read
        sta ngaEntry
        nga.read
        sta ngaEntry+1
        nga.read
        sta ngaCount            ; blocks that follow
.if ngaSelects
        nga.read                ; the ROMs the entered Phase needs, switched
        nga.select              ; before a byte of it lands
.endif
        lda #ngaFrameHeader
        jsr ngaFrameSkip
.if ngaFramesHeld
        lda ngaPtr
        ora ngaPtr+1
        beq @block              ; nowhere to hold them: one open a block

; A block's bytes, read in one pass from where the stream already stands. The
; write pointer, the counter and the index are Temporaries of the routine's own
; that nothing is keeping here: ngaValue belongs to the Cell writes, which come
; after the blocks, and ngaUnit and ngaDst to a block being loaded, which has
; not begun. Only ngaPtr is really spent, being live across the whole of the
; block loop and so no longer able to share with what a driver declares — which
; is the two bytes of zero page this way of fetching costs.
        lda ngaPtr
        sta ngaValue
        lda ngaPtr+1
        sta ngaValue+1
        lda ngaCount
        sta ngaUnit             ; blocks still to hold
@hold
        lda ngaUnit
        beq @heldBlock
        lda #0
        sta ngaDst              ; the index, which a read may take out of Y
@six
        nga.read
        ldy ngaDst
        sta (ngaValue),y
        iny
        sty ngaDst
        cpy #ngaBlockSize
        bne @six
        lda #ngaBlockSize
        jsr ngaFrameSkip        ; so the Frame stands past them at the end
        lda ngaValue
        clc
        adc #ngaBlockSize
        sta ngaValue
        bcc @counted
        inc ngaValue+1
@counted
        dec ngaUnit
        jmp @hold

; Then a block at a time out of memory, and the Frame opened once more at the
; end rather than once for every one of them.
@heldBlock
        lda ngaCount
        beq @cells
        ldy #0
        lda (ngaPtr),y
        sta nga.unit            ; straight into the driver's cells: the
        iny                     ; descriptors are in memory, and nothing
.if ngaUnitBytes > 1            ; reads the stream before it is opened
        lda (ngaPtr),y
        sta nga.unit+1
        iny
.endif
        lda (ngaPtr),y
        sta nga.offset
        iny
        lda (ngaPtr),y
        sta nga.offset+1
        iny
        lda (ngaPtr),y
        sta ngaDst
        iny
        lda (ngaPtr),y
        sta ngaDst+1
        iny
        lda (ngaPtr),y
        pha                     ; the decoder's number
        lda ngaPtr
        clc
        adc #ngaBlockSize
        sta ngaPtr
        bcc @heldLoad
        inc ngaPtr+1
@heldLoad
        nga.open
        pla
        ldx ngaDst
        ldy ngaDst+1
        jsr ngaTransform
        dec ngaCount
        jmp @heldBlock
.endif

@block
        lda ngaCount
        beq @cells
        jsr ngaFrameOpen        ; back to the Frame: a block's stream replaced it
        nga.read
        sta ngaUnit
.if ngaUnitBytes > 1
        nga.read
        sta ngaUnit+1
.endif
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
        lda #ngaBlockSize
        jsr ngaFrameSkip
        lda ngaUnit             ; only now: the driver reads the Frame through
        sta nga.unit            ; these very cells, and writing them while it
.if ngaUnitBytes > 1            ; did would move the stream
        lda ngaUnit+1
        sta nga.unit+1
.endif
        lda ngaOffset
        sta nga.offset
        lda ngaOffset+1
        sta nga.offset+1
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
        lda ngaWanted
        sta ngaCurrentPhase     ; the Phase the program is in from here on
        jmp (ngaEntry)
.endp

; The Frame's stream, from where the routine last left it.
.proc ngaFrameOpen
        lda ngaFrameAt
        sta nga.offset
        lda ngaFrameAt+1
        sta nga.offset+1
        lda ngaFrameAt+2
        sta nga.unit
.if ngaUnitBytes > 1
        lda ngaFrameAt+3
        sta nga.unit+1
.endif
        nga.open
        rts
.endp

; A bytes on in the Frame, which is where its stream is opened next. The
; offset carries into the unit, so that a Frame running past the last offset
; a unit's number can reach goes on into the next.
.proc ngaFrameSkip
        clc
        adc ngaFrameAt
        sta ngaFrameAt
        bcc @done
        inc ngaFrameAt+1
        bne @done
        inc ngaFrameAt+2
.if ngaUnitBytes > 1
        bne @done
        inc ngaFrameAt+3
.endif
@done
        rts
.endp
)asm";

constexpr std::string_view CELL_NAME = TRANSITION_CELL_NAME;

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

/// A unit's number or a Window's state, low byte first, in as many bytes as it
/// is written in where it is written — see Target::unitBytes and stateBytesOf.
void putNumber( std::span<std::uint8_t> into, std::size_t at, std::uint32_t value, std::uint32_t bytes )
{
  putByte( into, at, value );
  if ( bytes > 1 )
  {
    putByte( into, at + 1, value >> 8U );
  }
}

void putWord( std::span<std::uint8_t> into, std::size_t at, std::uint32_t value )
{
  putByte( into, at, value );
  putByte( into, at + 1, value >> 8 );
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

  // A Container with no loader enters its entry Phase the way any Phase is
  // entered — by the routine, from a Frame — so the routine is here wherever
  // it can be, edges or not. What that costs is in docs/spec/car.md.
  if ( !anyEdge && !takesColdStart( project ) )
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

  // The Cell: its pseudo-source is the one name it exports, so that the
  // Symbol's name is text the SourceManager owns like every other name.
  // Outside the Window, as every Cell is: the routine writes it with the
  // driver's stream open, which on a mapped driver is a Bank in.
  auto const addCell = [&]
  {
    diag::FileId const cellFile = sources.addFile( "<nga>/cell", std::string{ CELL_NAME } );
    ModuleIndex const cellModule = addModule( ProjectModule{ .name = "nga.cell",
                                                             .file = cellFile,
                                                             .residency = {},
                                                             .generated = Generated::TRANSITION_CELL,
                                                             .outsideWindow = true } );
    project.transitionCell = cellModule;
    return cellModule;
  };

  // Without storage there is nowhere for a Frame to wait and nothing for a
  // routine to read, so there is no routine: an edge is a jump to a stub of
  // the Phase it enters, which the end of Assemble adds once it knows which
  // Phases a statement enters. The Cell stays, since it is exported and the
  // stubs write it. See
  // docs/decisions/0234-without-storage-bytes-stay-where-they-are-live.md.
  if ( !project.target.hasStorage() )
  {
    if ( project.framesSite.has_value() )
    {
      sink.add( diag::diagnostic( diag::DiagnosticId::FRAMES_WITHOUT_STORAGE )
                    .at( project.framesSite->begin, project.framesSite->length ) );
    }
    ModuleIndex const cellModule = addCell();
    for ( Phase& phase : graph.phases )
    {
      addOnce( phase.needs, cellModule );
    }
    deriveResidency( project );
    return;
  }

  // The routine: the tool's own source, knowing no hardware. What it calls
  // is the driver's, under names the dispatcher defines at the end of
  // Assemble.
  // The routine's own text, with the one thing it asks of the Project in front
  // of it: whether an edge's descriptors are held. The text is built here,
  // before Assemble, so it can be asked of the Project and not of the driver,
  // which is resolved at the end of it — see docs/spec/project-file.md.
  // And how wide a unit's number is, which the variant says, with the sizes
  // that follow from it written in from the one place Patch and Size read them
  // too — see docs/decisions/0228-a-driver-names-the-cells-its-stream-stands-in.md.
  // Whether the driver switches ROMs is the one thing here that is the
  // driver's, so it is not written in but read as `ngaSelects`, which the
  // dispatcher's Module defines once the driver is resolved and `.if` answers
  // once every Module's Symbols are known — see
  // docs/decisions/0229-no-window-has-a-base.md.
  std::uint32_t const unitBytes = project.target.unitBytes();
  std::string routineText = fmt::format( "ngaFramesHeld = {}\n"
                                         "ngaUnitBytes = {}\n"
                                         "ngaEntrySize = {}\n"
                                         "ngaBlockSize = {}\n"
                                         "ngaFrameHeader = {} + ngaSelects\n",
                                         project.framesHeld ? 1 : 0,
                                         unitBytes,
                                         transitionEntrySize( unitBytes ),
                                         frameBlockSize( unitBytes ),
                                         frameHeaderSize( project.framesHeld ) );
  routineText += ROUTINE_SOURCE;
  if ( takesColdStart( project ) )
  {
    // The Phase to enter is known here; where its Frame waits is not, and is
    // three bytes the Container writes.
    routineText += fmt::format( COLD_START_SOURCE, graph.entry.value );
  }
  diag::FileId const routineFile = sources.addFile( "<nga>/transition.asm", routineText );
  ModuleIndex const routineModule = addModule( ProjectModule{ .name = "nga.transition",
                                                              .file = routineFile,
                                                              .residency = {},
                                                              .generated = Generated::NONE,
                                                              .outsideWindow = true } );
  project.transitionRoutine = routineModule;

  ModuleIndex const cellModule = addCell();

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

  Section section{ syntax::PlacementClass::ABSOLUTE, nullptr, nullptr, nullptr, false, SectionKind::PLAIN, span };
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
loadSetOf( PhaseGraph const& graph, std::optional<PhaseIndex> from, PhaseIndex to, std::span<Module const> modules )
{
  std::vector<SectionRef> loaded;
  for ( ModuleIndex const module : graph.phases[to.value].needs )
  {
    // The cold start comes from no Phase, so nothing was kept: everything the
    // entry Phase needs is the load set, and what stands in ROM is filtered
    // out where Payloads are marked.
    bool const kept = from.has_value() && std::ranges::find( graph.phases[from->value].needs, module ) !=
                                              graph.phases[from->value].needs.end();
    Module const& one = modules[module.value];
    for ( std::uint32_t index = 0; index < one.sections().size(); ++index )
    {
      // A Pane's Section is loaded once, by the Container into its Bank, and
      // no edge copies it — see docs/decisions/0054-panes.md — and so is one
      // in a unit set.
      Section const& section = one.sections()[index];
      if ( section.emitsBytes() && !section.inMedium() && !kept )
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
  // Without storage an edge loads nothing and has no Frame to count in.
  for ( std::uint32_t from = 0; project.target.hasStorage() && from < graph.phases.size(); ++from )
  {
    for ( PhaseIndex const next : graph.phases[from].then )
    {
      std::vector<SectionRef> const loaded = loadSetOf( graph, PhaseIndex{ from }, next, modules );
      if ( loaded.size() > MOST_BLOCKS )
      {
        std::string const fromName = graph.phases[from].displayName();
        std::string const toName = graph.phases[next.value].displayName();
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

  // The Phases a `.transition` naming no Phase may run in. Which edge it
  // meant is not known, so none of theirs is said to be taken by nothing:
  // the misspelt name is the one finding.
  std::vector<bool> misnamedIn( graph.phases.size(), false );

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
          for ( std::uint32_t phase = 0; phase < graph.phases.size(); ++phase )
          {
            misnamedIn[phase] = misnamedIn[phase] || module.residency().includes( PhaseIndex{ phase } );
          }
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
                          .arg( "from", graph.phases[phase].displayName() ) );
            complete = false;
          }
        }
        if ( complete )
        {
          transition->target = target;
        }
        // The statement stands in every Phase of its Module, so each of
        // those with the edge has a statement that takes it, whether or not
        // another Phase lacks the edge and refused the statement.
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

  for ( std::uint32_t from = 0; from < graph.phases.size(); ++from )
  {
    for ( PhaseIndex const to : graph.phases[from].then )
    {
      if ( taken[from][to.value] || misnamedIn[from] )
      {
        continue;
      }
      std::string const fromName = graph.phases[from].displayName();
      std::string const toName = graph.phases[to.value].displayName();
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

std::optional<std::uint32_t> startAddressOf( Placed const& build, diag::DiagnosticSink& sink )
{
  PhaseIndex const entry = build.phases().entry;
  diag::SeverityPolicy quietPolicy;
  diag::DiagnosticSink quiet{ quietPolicy };
  return entryAddressOf( build, entry, isEnteredByAnEdge( build.phases(), entry ) ? quiet : sink );
}

std::optional<std::uint32_t> entryAddressOf( Placed const& build, PhaseIndex phase, diag::DiagnosticSink& sink )
{
  GlobalSymbols const& symbols = build.symbols();
  Layout const& layout = build.layout();
  Sizes const& sizes = build.sizes();
  Phase const& entered = build.phases().phases[phase.value];
  std::string const name = entered.entry.value_or( "entry" );
  std::string const phaseName = entered.displayName();

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
  return layout.addressOf( where ) + sizes.offsetOf( where, position.chunk );
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

std::uint32_t sizeOfTransition( Residency const& residency, std::uint32_t unitBytes )
{
  std::uint32_t phases = 0;
  for ( std::uint32_t phase = 0; phase < residency.phaseCount(); ++phase )
  {
    if ( residency.includes( PhaseIndex{ phase } ) )
    {
      ++phases;
    }
  }
  return TRANSITION_HEADER_SIZE + ( transitionEntrySize( unitBytes ) * phases );
}

std::uint32_t
sizeOfFrame( std::uint32_t payloads, std::uint32_t cellWrites, std::uint32_t headerBytes, std::uint32_t unitBytes )
{
  return headerBytes + ( frameBlockSize( unitBytes ) * payloads ) + 1 + ( CELL_WRITE_SIZE * cellWrites );
}

std::uint32_t frameHeaderSizeOf( Project const& project )
{
  bool const selects = project.driver.has_value() && project.driver->select.has_value();
  return frameHeaderSize( project.framesHeld ) + ( selects ? 1 : 0 );
}

std::uint32_t romsOf( Project const& project, PhaseIndex phase )
{
  std::uint32_t set = 0;
  if ( !project.driver.has_value() )
  {
    return set;
  }
  std::vector<ModuleIndex> const& needs = project.phases.phases[phase.value].needs;
  for ( std::size_t bit = 0; bit < project.driver->roms.size(); ++bit )
  {
    std::optional<ModuleIndex> const module = project.driver->roms[bit].module;
    if ( module.has_value() && std::ranges::find( needs, *module ) != needs.end() )
    {
      set |= 1U << bit;
    }
  }
  return set;
}

std::uint32_t stateBytesOf( Project const& project, WindowIndex window )
{
  if ( !project.driver.has_value() || window.value >= project.driver->windows.size() )
  {
    return 1;
  }
  return project.driver->windows[window.value].stateBytes;
}

bool fillsNamedStates( Project const& project, WindowIndex window )
{
  return project.container == Container::XEX && project.driver.has_value() && project.driver->stream == window;
}

std::string nameOfFrame( PhaseGraph const& graph, std::optional<PhaseIndex> from, PhaseIndex to )
{
  std::string const entered = graph.phases[to.value].displayName();
  if ( !from.has_value() )
  {
    return "frame into " + entered;
  }
  return "frame " + graph.phases[from->value].displayName() + " -> " + entered;
}

void writeTransition( TransitionContent const& content,
                      ModuleIndex home,
                      Placed const& build,
                      Storage const& storage,
                      std::span<std::uint8_t> into )
{
  // Without storage the statement is a jump to the stub of the Phase it
  // enters, which is everything a Frame would have said, written as code.
  if ( !build.project().target.hasStorage() )
  {
    std::uint32_t const stub =
        content.target.has_value()
            ? addressOfExported( build.symbols(), entryStubName( *content.target ), build.sizes(), build.layout() )
                  .value_or( 0 )
            : 0;
    putByte( into, 0, 0x4C ); // jmp
    putWord( into, 1, stub );
    return;
  }
  Residency const& residency = build.symbols().moduleAt( home ).residency();
  std::uint32_t const unitBytes = build.project().target.unitBytes();
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
    putNumber( into, at + 1, known ? waits.bank.value : 0, unitBytes );
    putWord( into, at + 1 + unitBytes, known ? waits.offset : 0 );
    at += transitionEntrySize( unitBytes );
    ++entries;
  }
  putByte( into, 4, entries );
}

std::vector<EdgeBlock> edgeBlocksOf( FrameIndex frame, Placed const& build, Storage const& storage )
{
  Layout const& layout = build.layout();

  // What the edge copies: its load set.
  std::vector<EdgeBlock> blocks;
  for ( SectionRef const payload : storage.framePayloadsOf( frame ) )
  {
    // Sized for the whole load set; what is listed is what Prune kept.
    if ( !build.reachable().includes( payload ) )
    {
      continue;
    }
    // A block per **piece**. A Payload is one stored form unless no unit of
    // storage held the whole of it, and then it is several, each a form the
    // decoder reads from its first byte and each with a destination of its
    // own.
    for ( std::uint32_t piece = 0; piece < storage.pieceCountOf( payload ); ++piece )
    {
      blocks.push_back( EdgeBlock{ .payload = payload, .piece = piece } );
    }
  }

  if ( !build.project().framesHeld )
  {
    return blocks;
  }

  // The block that lends the routine its list of descriptors goes last. It is
  // the longest of them, and it serves only if it is longer than the list —
  // a block's bytes for each — which an edge of many short Sections may leave nobody
  // able to do. What is compared is the **stored form**, never the longer of
  // the two, so a block with room for the list as it waits has room for it
  // once turned back, whichever decoder turns it.
  auto const need = static_cast<std::uint32_t>( frameBlockSize( build.project().target.unitBytes() ) * blocks.size() );
  std::optional<std::size_t> lender;
  std::uint32_t largest = 0;
  for ( std::size_t index = 0; index < blocks.size(); ++index )
  {
    if ( !layout.isPlaced( blocks[index].payload ) )
    {
      continue;
    }
    // The **last** of the longest, so that blocks of one size leave the order
    // alone and nothing is rotated for nothing.
    std::uint32_t const room = storage.sizeOf( blocks[index].payload, blocks[index].piece );
    if ( room >= need && room >= largest )
    {
      largest = room;
      lender = index;
    }
  }
  if ( lender.has_value() )
  {
    auto const first = blocks.begin() + static_cast<std::ptrdiff_t>( *lender );
    std::rotate( first, first + 1, blocks.end() );
  }
  return blocks;
}

namespace
{

/// The Cells entering `phase` rewrites, each with what it is written: every
/// Slot with an Implementation live there gets its Cell set to that
/// Implementation's address. A `vector` Cell keeps its `jmp` and is written
/// one byte in. A Frame and an entry stub write the same.
std::vector<std::pair<std::uint32_t, std::uint32_t>> cellWritesOf( Placed const& build, PhaseIndex phase )
{
  GlobalSymbols const& symbols = build.symbols();
  Layout const& layout = build.layout();
  std::vector<std::pair<std::uint32_t, std::uint32_t>> writes;
  for ( SymbolRef const slot : slotsOf( build.modules() ) )
  {
    auto const& declaration = std::get<SlotDeclaration>( symbols.at( slot ).value );
    std::optional<SymbolRef> const live = implementationIn( symbols, slot, phase );
    if ( !declaration.cell.has_value() || !live.has_value() || !layout.isPlaced( *declaration.cell ) )
    {
      continue;
    }
    std::uint32_t const cell =
        layout.addressOf( *declaration.cell ) + ( declaration.binding == syntax::Binding::VECTOR ? 1 : 0 );
    writes.emplace_back( cell, addressOfImplementation( build, *live ).value_or( 0 ) );
  }
  return writes;
}

} // namespace

std::vector<std::uint8_t>
frameBytesOf( FrameIndex frame, Placed const& build, Storage const& storage, EntryAddresses& entries )
{
  Layout const& layout = build.layout();

  std::vector<std::uint8_t> bytes( storage.frameSizeOf( frame ), 0 );
  std::span<std::uint8_t> const into{ bytes };
  std::size_t at = 0;
  PhaseIndex const to = storage.frameTo( frame );
  std::uint32_t const unitBytes = build.project().target.unitBytes();
  std::uint32_t const blockSize = frameBlockSize( unitBytes );

  std::vector<EdgeBlock> const blocks = edgeBlocksOf( frame, build, storage );

  // Where the descriptors wait while the images arrive: the destination of the
  // last block, which edgeBlocksOf put there because it is the largest and has
  // room for them — and zero where no block of this edge had. Its bytes are
  // overwritten by this edge in any case, so until it arrives what stands
  // there is nobody's, and the list costs the program no memory at all. See
  // docs/decisions/0226-an-edge-holds-its-descriptors-in-the-block-that-loads-last.md.
  if ( build.project().framesHeld )
  {
    bool const lent = !blocks.empty() && layout.isPlaced( blocks.back().payload ) &&
                      storage.sizeOf( blocks.back().payload, blocks.back().piece ) >= blockSize * blocks.size();
    putWord( into,
             at,
             lent ? layout.addressOf( blocks.back().payload ) +
                        storage.landingOf( blocks.back().payload, blocks.back().piece )
                  : 0 );
    at += FRAME_HELD_SIZE;
  }
  putWord( into, at, entries.of( to ).value_or( 0 ) );
  putByte( into, at + 2, static_cast<std::uint32_t>( blocks.size() ) );
  at += FRAME_HEADER_SIZE;

  // The ROMs the entered Phase needs, which the routine hands the driver
  // before the first block lands, since a Payload for RAM under a ROM lands
  // only once the ROM is out — see docs/decisions/0229-no-window-has-a-base.md.
  if ( build.project().driver.has_value() && build.project().driver->select.has_value() )
  {
    putByte( into, at, romsOf( build.project(), to ) );
    at += 1;
  }

  for ( auto const& [payload, piece] : blocks )
  {
    bool const known = storage.isPlaced( payload, piece ) && layout.isPlaced( payload );
    StorageAddress const waits = known ? storage.addressOf( payload, piece ) : StorageAddress{};
    putNumber( into, at, known ? waits.bank.value : 0, unitBytes );
    putWord( into, at + unitBytes, known ? waits.offset : 0 );
    // Where the decoder's output lands: the Section's address in the Phase
    // entered plus how far into it this piece begins, which is Storage's
    // answer because the Payload was made from the initialised extent and not
    // from the Section; the stored size travels with the form.
    putWord( into, at + unitBytes + 2, known ? layout.addressOf( payload ) + storage.landingOf( payload, piece ) : 0 );
    putByte( into, at + unitBytes + 4, known ? storage.transformOf( payload ) : TRANSFORM_COPY );
    at += blockSize;
  }

  // The Cells. The Frame was sized for every Slot, and a shorter list leaves
  // the rest unread.
  std::vector<std::pair<std::uint32_t, std::uint32_t>> const writes = cellWritesOf( build, to );
  putByte( into, at, static_cast<std::uint32_t>( writes.size() ) );
  at += 1;
  for ( auto const& [cell, value] : writes )
  {
    putWord( into, at, cell );
    putWord( into, at + 2, value );
    at += CELL_WRITE_SIZE;
  }
  return bytes;
}

void writeSlotCell( SlotCellContent const& content, Placed const& build, std::span<std::uint8_t> into )
{
  PhaseGraph const& graph = build.phases();
  SymbolRef const slot{ .module = content.module, .index = content.symbol };
  std::optional<SymbolRef> const live = implementationIn( build.symbols(), slot, graph.entry );
  std::uint32_t const value = live.has_value() ? addressOfImplementation( build, *live ).value_or( 0 ) : 0;
  if ( content.binding == syntax::Binding::VECTOR )
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

std::string entryStubName( PhaseIndex phase )
{
  return std::string{ ENTRY_STUB_PREFIX } + std::to_string( phase.value );
}

namespace
{

/// `lda #imm`, `sta abs`, `jsr abs` and `jmp abs`, and how long each is.
constexpr std::uint8_t LDA_IMMEDIATE = 0xA9;
constexpr std::uint8_t STA_ABSOLUTE = 0x8D;
constexpr std::uint8_t JSR = 0x20;
constexpr std::uint8_t JMP = 0x4C;
constexpr std::uint32_t SELECT_SIZE = 5;
constexpr std::uint32_t SLOT_WRITE_SIZE = 10;
constexpr std::uint32_t PHASE_WRITE_SIZE = 5;

bool selectsRoms( Project const& project )
{
  return project.driver.has_value() && project.driver->select.has_value();
}

} // namespace

std::uint32_t sizeOfEntryStub( Project const& project, std::uint32_t slots )
{
  return ( selectsRoms( project ) ? SELECT_SIZE : 0 ) + ( SLOT_WRITE_SIZE * slots ) + PHASE_WRITE_SIZE +
         TRANSITION_JUMP_SIZE;
}

void addEntryStubs( diag::SourceManager& sources,
                    Project& project,
                    std::vector<Module>& modules,
                    diag::DiagnosticSink& sink )
{
  if ( project.target.hasStorage() || !project.transitionCell.has_value() )
  {
    return;
  }
  PhaseGraph& graph = project.phases;
  std::size_t const phaseCount = graph.phases.size();

  // Where each Phase is entered from: every Phase in which a statement that
  // enters it may run, which is the Residency of the statement's Module.
  std::vector<Residency> takenIn( phaseCount, Residency{ phaseCount } );
  std::vector<bool> entered( phaseCount, false );
  for ( Module const& module : modules )
  {
    for ( Section const& section : module.sections() )
    {
      for ( Chunk const& chunk : section.chunks() )
      {
        auto const* const transition = std::get_if<TransitionContent>( &chunk.content );
        if ( transition == nullptr || !transition->target.has_value() )
        {
          continue;
        }
        std::uint32_t const target = transition->target->value;
        entered[target] = true;
        for ( std::uint32_t phase = 0; phase < phaseCount; ++phase )
        {
          if ( module.residency().includes( PhaseIndex{ phase } ) )
          {
            takenIn[target].add( PhaseIndex{ phase } );
          }
        }
      }
    }
  }
  if ( std::ranges::none_of( entered, []( bool one ) { return one; } ) )
  {
    return;
  }

  auto const list = [&project, phaseCount]( ModuleIndex index, Residency const& residency )
  {
    for ( std::uint32_t phase = 0; phase < phaseCount; ++phase )
    {
      if ( residency.includes( PhaseIndex{ phase } ) )
      {
        addOnce( project.phases.phases[phase].needs, index );
      }
    }
  };

  // The driver's `select`, which a stub calls since Patch writes a stub's
  // bytes and cannot expand a macro into them. Where the driver is, which is
  // not in a `final` Phase other than the entry — see
  // docs/decisions/0245-a-final-phase-keeps-nothing-only-an-exit-runs.md.
  if ( selectsRoms( project ) )
  {
    std::string source = "; The driver's `select`, wrapped so that a Phase's stub has an address to\n"
                         "; call. See docs/decisions/0234-without-storage-bytes-stay-where-they-are-live.md.\n"
                         ".export ";
    source += SELECT_ROMS_NAME;
    source += "\n.proc ";
    source += SELECT_ROMS_NAME;
    source += "\n        nga.select\n        rts\n.endp\n";
    addAssembledModule( sources, project, modules, "nga.roms", std::move( source ), notFinal( project.phases ), sink );
  }

  // A stub per Phase entered. It holds bytes the Container writes at load and
  // nothing writes again, so it is present where a statement into its Phase
  // runs and in every Phase from which one of those is reached, which is what
  // keeps any edge from having to bring it.
  for ( std::uint32_t phase = 0; phase < phaseCount; ++phase )
  {
    if ( !entered[phase] )
    {
      continue;
    }
    Residency const present = liveAcross( graph, takenIn[phase] );
    std::string const name = entryStubName( PhaseIndex{ phase } );
    diag::FileId const file = sources.addFile( "<nga>/" + name, name );
    diag::SourceSpan const span{ .begin = sources.locationOf( file, 0 ), .length = 0 };
    std::string const moduleName = "nga.into." + graph.phases[phase].name.value_or( std::to_string( phase ) );
    Module module{ moduleName, file, present };
    Section section{ syntax::PlacementClass::ABSOLUTE, nullptr, nullptr, nullptr, false, SectionKind::PLAIN, span };
    section.appendChunk( PhaseEntryContent{ .phase = PhaseIndex{ phase } }, span, {} );
    SectionIndex const at = module.addSection( std::move( section ) );
    diag::SourceSpan const label{ .begin = sources.locationOf( file, 0 ),
                                  .length = static_cast<std::uint32_t>( name.size() ) };
    module.symbols().add(
        Symbol{ .name = sources.textOf( label ),
                .kind = SymbolKind::LABEL,
                .definition = label,
                .exported = true,
                .value = LabelPosition{ .section = at, .chunk = ChunkIndex{ 0 }, .inner = std::nullopt } } );
    module.keepOutsideWindow();
    ModuleIndex const index{ static_cast<std::uint32_t>( modules.size() ) };
    modules.push_back( std::move( module ) );
    project.modules.push_back( ProjectModule{ .name = moduleName,
                                              .file = file,
                                              .residency = present,
                                              .generated = Generated::ENTRY_STUB,
                                              .outsideWindow = true } );
    list( index, present );
  }
}

void writeEntryStub( PhaseEntryContent const& content,
                     Placed const& build,
                     std::optional<std::uint32_t> entry,
                     std::span<std::uint8_t> into )
{
  GlobalSymbols const& symbols = build.symbols();
  Layout const& layout = build.layout();
  std::size_t at = 0;
  auto const op = [&into, &at]( std::uint8_t opcode, std::uint32_t operand, std::uint32_t width )
  {
    putByte( into, at, opcode );
    if ( width == 1 )
    {
      putByte( into, at + 1, operand );
    }
    else
    {
      putWord( into, at + 1, operand );
    }
    at += 1 + width;
  };

  // The ROMs first, as the routine hands them over before anything of the
  // Phase lands — see docs/decisions/0229-no-window-has-a-base.md.
  if ( selectsRoms( build.project() ) )
  {
    op( LDA_IMMEDIATE, romsOf( build.project(), content.phase ), 1 );
    op( JSR, addressOfExported( symbols, SELECT_ROMS_NAME, build.sizes(), layout ).value_or( 0 ), 2 );
  }

  // The Cells the Frame would have written.
  for ( auto const& [cell, value] : cellWritesOf( build, content.phase ) )
  {
    op( LDA_IMMEDIATE, value, 1 );
    op( STA_ABSOLUTE, cell, 2 );
    op( LDA_IMMEDIATE, value >> 8U, 1 );
    op( STA_ABSOLUTE, cell + 1, 2 );
  }

  op( LDA_IMMEDIATE, content.phase.value, 1 );
  op( STA_ABSOLUTE, addressOfExported( symbols, CELL_NAME, build.sizes(), layout ).value_or( 0 ), 2 );
  op( JMP, entry.value_or( 0 ), 2 );
}

std::string underGlueName( std::string_view format )
{
  return "ngaUnder" + loadGlueName( format ).substr( std::string_view{ "nga" }.size() );
}

std::string loadGlueName( std::string_view format )
{
  std::string name{ "ngaUnpack" };
  std::size_t const first = name.size();
  name += format;
  if ( name.size() > first && name[first] >= 'a' && name[first] <= 'z' )
  {
    name[first] = static_cast<char>( name[first] - 'a' + 'A' );
  }
  return name;
}

} // namespace nga::model
