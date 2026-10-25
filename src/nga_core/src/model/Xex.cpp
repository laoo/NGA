#include "nga/model/Emit.hpp"

#include "nga/model/Load.hpp"
#include "nga/model/Place.hpp"
#include "nga/model/Prune.hpp"
#include "nga/model/Segments.hpp"
#include "nga/model/Transform.hpp"
#include "nga/model/Transition.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <future>
#include <map>
#include <optional>
#include <span>
#include <thread>
#include <tuple>
#include <utility>
#include <vector>

namespace nga::model
{

namespace
{

/// Where an `init` of the `load` block points: a Label of a Module the load
/// needs, defined once. Reported where it is not, as an entry is.
std::optional<std::uint32_t> loadInitAddress( Patched const& build, LoadInit const& init, diag::DiagnosticSink& sink )
{
  GlobalSymbols const& symbols = build.symbols();
  Symbol const* found = nullptr;
  ModuleIndex home{};
  bool twice = false;
  for ( ModuleIndex const module : build.project().loadNeeds )
  {
    if ( Symbol const* const candidate = symbols.moduleAt( module ).symbols().find( init.name ); candidate != nullptr )
    {
      twice = twice || found != nullptr;
      found = candidate;
      home = module;
    }
  }
  std::string_view what;
  if ( found == nullptr )
  {
    what = "nothing";
  }
  else if ( twice )
  {
    what = "more than one definition";
  }
  else if ( found->kind != SymbolKind::LABEL )
  {
    what = "no label";
  }
  if ( !what.empty() )
  {
    sink.add( diag::diagnostic( diag::DiagnosticId::LOAD_INIT_UNKNOWN )
                  .at( init.span.begin, init.span.length )
                  .arg( "name", init.name )
                  .arg( "what", std::string{ what } ) );
    return std::nullopt;
  }
  LabelPosition const position = std::get<LabelPosition>( found->value );
  SectionRef const where{ .module = home, .section = position.section };
  if ( !build.layout().isPlaced( where ) || !build.sizes().isKnown( where ) )
  {
    return std::nullopt;
  }
  return build.layout().addressOf( where ) + build.sizes().offsetOf( where, position.chunk );
}

/// Which bytes of a Section some bytes of a segment are: those from `at`, for
/// `size`, are the Section's from `offset`. What a `.xex` built from a
/// `choose` block finds the bytes it changes by, wherever the file has moved,
/// cut or joined them — see
/// docs/decisions/0302-a-document-may-be-chosen-while-a-xex-loads.md.
struct Carried
{
  SectionRef where{};
  std::uint32_t offset = 0;
  std::uint32_t at = 0;
  std::uint32_t size = 0;
};

/// What of `carried` lies between `begin` and `end`.
std::vector<Carried> clipped( std::span<Carried const> carried, std::uint32_t begin, std::uint32_t end )
{
  std::vector<Carried> inside;
  for ( Carried const& one : carried )
  {
    std::uint32_t const low = std::max( one.at, begin );
    std::uint32_t const high = std::min( one.at + one.size, end );
    if ( low < high )
    {
      inside.push_back(
          Carried{ .where = one.where, .offset = one.offset + ( low - one.at ), .at = low, .size = high - low } );
    }
  }
  return inside;
}

/// One segment of the file before it is written: where it lands, its bytes,
/// and whether a compressed file may carry it encoded. A cell, a word at
/// `INITAD` and a form already encoded may not. What of Sections it carries as
/// they are, and for a word at `INITAD` that calls a decoder, what of them the
/// decode writes.
struct Piece
{
  std::uint32_t at = 0;
  std::vector<std::uint8_t> bytes;
  bool compressible = true;
  std::vector<Carried> carried{};
  std::vector<Carried> decodes{};

  [[nodiscard]] std::uint32_t end() const
  {
    return at + static_cast<std::uint32_t>( bytes.size() );
  }

  [[nodiscard]] bool callsInit() const
  {
    return at <= INITAD + 1 && end() > INITAD;
  }
};

/// Two bytes at `at`, the low one first.
Piece wordPiece( std::uint32_t at, std::uint32_t value )
{
  return Piece{ .at = at,
                .bytes = { static_cast<std::uint8_t>( value & 0xFF ),
                           static_cast<std::uint8_t>( ( value >> 8 ) & 0xFF ) },
                .compressible = false,
                .carried = {},
                .decodes = {} };
}

/// The file as a list of segments in the order the loader takes them, which
/// is written only once the compression has had its say. Without one it is
/// written as it was composed, and the segments join as they always have.
class Plan
{
public:
  void segment( std::uint32_t at,
                std::span<std::uint8_t const> data,
                bool compressible = true,
                std::vector<Carried> carried = {} )
  {
    if ( !data.empty() )
    {
      mPieces.push_back( Piece{ .at = at,
                                .bytes = { data.begin(), data.end() },
                                .compressible = compressible,
                                .carried = std::move( carried ),
                                .decodes = {} } );
    }
  }

  /// The bytes of a Section, from `offset` into it.
  void section( std::uint32_t at,
                std::span<std::uint8_t const> data,
                SectionRef where,
                std::uint32_t offset,
                bool compressible = true )
  {
    segment(
        at,
        data,
        compressible,
        { Carried{ .where = where, .offset = offset, .at = at, .size = static_cast<std::uint32_t>( data.size() ) } } );
  }

  /// `INITAD` at a decoder, which writes `decoded` as it returns.
  void decode( std::uint32_t glue, Carried decoded )
  {
    Piece call = wordPiece( INITAD, glue );
    call.decodes.push_back( decoded );
    mPieces.push_back( std::move( call ) );
  }

  void byte( std::uint32_t at, std::uint8_t value )
  {
    std::array<std::uint8_t, 1> const one{ value };
    segment( at, one, false );
  }

  void word( std::uint32_t at, std::uint32_t value )
  {
    mPieces.push_back( wordPiece( at, value ) );
  }

  /// From here on `nga.load` is in memory, and what follows may be decoded.
  void decoderWritten()
  {
    mDecoderAt = mPieces.size();
  }

  [[nodiscard]] std::vector<Piece> const& pieces() const
  {
    return mPieces;
  }

  [[nodiscard]] std::optional<std::size_t> decoderAt() const
  {
    return mDecoderAt;
  }

private:
  std::vector<Piece> mPieces;
  std::optional<std::size_t> mDecoderAt;
};

/// A `.xex` begins with $FF $FF and is otherwise the segments.
std::vector<std::uint8_t> fileOf( std::span<Piece const> pieces )
{
  SegmentWriter out;
  for ( Piece const& piece : pieces )
  {
    out.segment( piece.at, piece.bytes );
  }
  std::vector<std::uint8_t> file{ 0xFF, 0xFF };
  std::vector<std::uint8_t> const segments = std::move( out ).take();
  file.insert( file.end(), segments.begin(), segments.end() );
  return file;
}

/// The file's segments, in the order of docs/spec/xex.md, or false where it
/// cannot be written. `compressing` puts the rest of the start after the units
/// and writes `nga.load` for the decodes — see
/// docs/decisions/0259-a-xex-may-be-compressed-segment-by-segment.md.
bool composeXex( Patched const& build, Plan& out, bool compressing, diag::DiagnosticSink& sink )
{
  Project const& project = build.project();
  GlobalSymbols const& symbols = build.symbols();
  Sizes const& sizes = build.sizes();
  Layout const& layout = build.layout();
  Target const& target = build.target();

  // What `except` names is written as it is, and where a file without
  // compression writes it — see
  // docs/decisions/0260-a-compressed-xex-writes-what-it-is-told-as-it-is.md.
  auto const compressible = [&project]( std::optional<SectionRef> where )
  {
    return !where.has_value() || std::ranges::find( project.uncompressed, where->module ) == project.uncompressed.end();
  };

  std::vector<Stored> const stored = storedImages( build );

  // The program's own code for the load first, and each `init` through
  // `INITAD`, in order: it runs before anything else of the file is written,
  // so what follows may stand over it — see
  // docs/decisions/0246-the-load-of-a-xex-needs-what-the-machine-holds-then.md.
  // What of it a Holder holds into a Phase is written here too, where Place
  // put it, and nothing below stands on it — see
  // docs/decisions/0247-the-hardware-holds-a-section-for-as-long-as-it-is-given-it.md.
  for ( ModuleIndex const module : project.loadNeeds )
  {
    Module const& one = build.modules()[module.value];
    for ( std::uint32_t index = 0; index < one.sections().size(); ++index )
    {
      SectionRef const where{ .module = module, .section = SectionIndex{ index } };
      // A Section in a Pane or a unit set goes in with the units, as every
      // other one does — see
      // docs/decisions/0261-a-section-of-the-load-in-a-medium-is-written-with-the-units.md.
      if ( isMachineAtLoad( build, where ) || !one.sections()[index].emitsBytes() || one.sections()[index].inMedium() ||
           !layout.isPlaced( where ) || !sizes.isKnown( where ) )
      {
        continue;
      }
      InitialisedExtent const extent = sizes.initialisedExtentOf( where );
      out.section(
          layout.addressOf( where ) + extent.begin, contentOf( build.bytes(), where, extent ), where, extent.begin );
    }
  }
  // What chooses and changes the bytes for a Choice, before the first `init`
  // may write the byte it chooses in — see
  // docs/decisions/0302-a-document-may-be-chosen-while-a-xex-loads.md.
  if ( std::optional<ModuleIndex> const choose = project.chooseModule; choose.has_value() )
  {
    Module const& module = build.modules()[choose->value];
    for ( std::uint32_t index = 0; index < module.sections().size(); ++index )
    {
      SectionRef const where{ .module = *choose, .section = SectionIndex{ index } };
      if ( !module.sections()[index].emitsBytes() || !layout.isPlaced( where ) || !sizes.isKnown( where ) )
      {
        continue;
      }
      InitialisedExtent const extent = sizes.initialisedExtentOf( where );
      out.segment( layout.addressOf( where ) + extent.begin, contentOf( build.bytes(), where, extent ), false );
    }
  }
  for ( LoadInit const& init : project.loadInits )
  {
    std::optional<std::uint32_t> const address = loadInitAddress( build, init, sink );
    if ( !address.has_value() )
    {
      return false;
    }
    out.word( INITAD, *address );
  }

  // A Section carried encoded is decoded in place while the file loads, before
  // anything else of the program is written: the decoders first, then each
  // form loaded against the top of its Section plus its margin, the two words
  // that say where it stands and where it goes, and INITAD at the format's
  // glue — in ascending order of address, so that no form's margin reaches a
  // Section decoded before it. What is written after may stand over the
  // decoders, which are done with by then — see
  // docs/decisions/0235-a-xex-decodes-in-place-while-it-loads.md.
  std::span<Storage::LoadForm const> const forms = build.storage().loadForms();
  std::span<Storage::HiddenBatch const> const batches = build.storage().hiddenBatches();
  std::span<Storage::HiddenForm const> const hiddenForms = build.storage().hiddenForms();
  if ( !forms.empty() || !batches.empty() || !hiddenForms.empty() || compressing )
  {
    std::optional<ModuleIndex> const load = project.loadModule;
    if ( !load.has_value() )
    {
      return false;
    }
    Module const& module = build.modules()[load->value];
    for ( std::uint32_t index = 0; index < module.sections().size(); ++index )
    {
      SectionRef const where{ .module = *load, .section = SectionIndex{ index } };
      if ( !module.sections()[index].emitsBytes() || !layout.isPlaced( where ) || !sizes.isKnown( where ) )
      {
        continue;
      }
      InitialisedExtent const extent = sizes.initialisedExtentOf( where );
      out.segment( layout.addressOf( where ) + extent.begin, contentOf( build.bytes(), where, extent ), false );
    }
    out.decoderWritten();

    // What hides under a ROM goes there first, batch by batch, while nothing
    // of the program stands anywhere the copies could be in the way of: each
    // batch's table and pieces, the table's address into the cell, and
    // INITAD at the move — see
    // docs/decisions/0241-bytes-kept-for-a-later-phase-may-lie-hidden-under-a-rom.md.
    if ( !batches.empty() || !hiddenForms.empty() )
    {
      if ( !project.driver.has_value() )
      {
        return false;
      }
      // The move switches the ROMs through the driver, whose code has to be
      // in memory by then: its Module goes first, and not again below.
      for ( Resident const& one : residentSections( build, {} ) )
      {
        if ( one.where.module == project.driver->module )
        {
          out.section( one.address,
                       one.content,
                       one.where,
                       one.address - layout.addressOf( one.where ),
                       compressible( one.where ) );
        }
      }
    }
    if ( !batches.empty() )
    {
      std::optional<std::uint32_t> const under = addressOfExported( symbols, UNDER_NAME, sizes, layout );
      std::optional<std::uint32_t> const table = addressOfExported( symbols, UNDER_TABLE_NAME, sizes, layout );
      if ( !under.has_value() || !table.has_value() )
      {
        return false;
      }
      for ( Storage::HiddenBatch const& batch : batches )
      {
        std::vector<Carried> carried;
        carried.reserve( batch.pieces.size() );
        for ( Storage::HiddenBatch::Piece const& piece : batch.pieces )
        {
          carried.push_back(
              Carried{ .where = piece.where, .offset = piece.offset, .at = piece.at, .size = piece.size } );
        }
        out.segment( batch.at, batch.image, std::ranges::all_of( batch.holds, compressible ), std::move( carried ) );
        out.word( *table, batch.at );
        out.word( INITAD, *under );
      }
    }
    // What hides encoded is decoded under its ROM one Section at a time: the
    // ROMs to put back once, then per Section its form, the two words that
    // say where it stands and where it goes, and INITAD at the format's glue.
    if ( !hiddenForms.empty() )
    {
      std::optional<std::uint32_t> const putBack = addressOfExported( symbols, UNDER_PUT_BACK_NAME, sizes, layout );
      std::optional<std::uint32_t> const from = addressOfExported( symbols, UNPACK_FROM_NAME, sizes, layout );
      std::optional<std::uint32_t> const into = addressOfExported( symbols, UNPACK_TO_NAME, sizes, layout );
      if ( !putBack.has_value() || !from.has_value() || !into.has_value() )
      {
        return false;
      }
      auto const roms = static_cast<std::uint8_t>( romsOf( project, project.phases.entry ) );
      out.byte( *putBack, roms );
      for ( Storage::HiddenForm const& form : hiddenForms )
      {
        std::optional<std::uint32_t> const glue =
            addressOfExported( symbols, underGlueName( form.format ), sizes, layout );
        if ( !glue.has_value() )
        {
          return false;
        }
        out.segment( form.at, form.form, false );
        out.word( *from, form.at );
        out.word( *into, form.to );
        out.word( INITAD, *glue );
      }
    }

    if ( !forms.empty() )
    {
      std::optional<std::uint32_t> const from = addressOfExported( symbols, UNPACK_FROM_NAME, sizes, layout );
      std::optional<std::uint32_t> const into = addressOfExported( symbols, UNPACK_TO_NAME, sizes, layout );
      if ( !from.has_value() || !into.has_value() )
      {
        return false;
      }
      for ( Storage::LoadForm const& form : forms )
      {
        std::optional<std::uint32_t> const glue =
            addressOfExported( symbols, loadGlueName( form.format ), sizes, layout );
        if ( !glue.has_value() )
        {
          return false;
        }
        out.segment( form.at, form.form, false );
        out.word( *from, form.at );
        out.word( *into, form.to );
        out.decode( *glue,
                    Carried{ .where = form.where,
                             .offset = form.to - layout.addressOf( form.where ),
                             .at = form.to,
                             .size = sizes.initialisedExtentOf( form.where ).size() } );
      }
    }
  }

  // Present when the program starts: everything the entry Phase needs that
  // emits bytes, in Project order. Before the units, because filling one
  // runs the driver, which is among them — unless the move under a ROM wrote
  // the driver already.
  std::vector<ModuleIndex> written{ project.loadNeeds.begin(), project.loadNeeds.end() };
  if ( ( !batches.empty() || !hiddenForms.empty() ) && project.driver.has_value() )
  {
    written.push_back( project.driver->module );
  }
  // Compressed, only what the loader calls goes before the units, and the
  // rest after them, so that the decoder may stand where it will — see
  // docs/decisions/0259-a-xex-may-be-compressed-segment-by-segment.md. What
  // `except` names stays where it was, since it is there to arrive early.
  std::vector<Resident> const start = residentSections( build, written );
  std::optional<Reachable> const called =
      compressing ? std::optional<Reachable>{ calledByTheLoader( build ) } : std::nullopt;
  std::vector<Resident const*> later;
  for ( Resident const& one : start )
  {
    if ( called.has_value() && !called->includes( one.where ) && compressible( one.where ) )
    {
      later.push_back( &one );
      continue;
    }
    out.section(
        one.address, one.content, one.where, one.address - layout.addressOf( one.where ), compressible( one.where ) );
  }

  // What the hardware takes before the driver can be used at all: INITAD at
  // the Proc that holds the driver's `init`, once, after the bytes that Proc
  // is among and before anything that calls the driver — the units among
  // them. Written only where
  // the driver declares the role, and then whether or not a unit is filled: a
  // program with no Payload still reaches the hardware through a `.with`.
  if ( project.driver.has_value() && project.driver->init.has_value() )
  {
    std::optional<std::uint32_t> const init = addressOfExported( symbols, INIT_NAME, sizes, layout );
    if ( !init.has_value() )
    {
      sink.add( diag::diagnostic( diag::DiagnosticId::XEX_INIT_NOT_PLACED ).sortedBy( "init" ) );
      return false;
    }
    out.word( INITAD, *init );
  }

  std::vector<StateStored> const states = stateImagesOf( build );
  if ( !stored.empty() || !states.empty() )
  {
    // The units are filled through the driver: a byte naming the unit's
    // state of the Window into the cell the dispatcher keeps for it, INITAD
    // at the glue that hands the byte to `showAt`, then the unit's images
    // into the window, which the program starts with showing the last unit
    // filled: what a Window shows is undefined until a `.with` says — see
    // docs/decisions/0229-no-window-has-a-base.md. Found here, when the
    // Container that needs them is written.
    std::optional<std::uint32_t> const loadUnit = addressOfExported( symbols, LOAD_UNIT_NAME, sizes, layout );
    std::optional<std::uint32_t> const loadMap = addressOfExported( symbols, LOAD_MAP_NAME, sizes, layout );
    if ( !project.driver.has_value() )
    {
      sink.add( diag::diagnostic( diag::DiagnosticId::XEX_WITHOUT_DRIVER ).sortedBy( "driver" ) );
      return false;
    }
    // Through the Window the driver's stream reads, whose units are storage's
    // where there is storage and a Pane's Banks either way — see
    // docs/decisions/0234-without-storage-bytes-stay-where-they-are-live.md.
    std::optional<UnitSetIndex> const filled = target.filledSet();
    if ( !project.driver->stream.has_value() || ( !stored.empty() && !filled.has_value() ) || !loadUnit.has_value() ||
         !loadMap.has_value() )
    {
      sink.add( diag::diagnostic( diag::DiagnosticId::XEX_NEEDS_WINDOW ).sortedBy( "window" ) );
      return false;
    }
    Window const& window = target.windows[project.driver->stream->value];
    if ( window.ranges.size() != 1 )
    {
      sink.add( diag::diagnostic( diag::DiagnosticId::XEX_WINDOW_NOT_ONE_RANGE )
                    .arg( "name", window.name )
                    .arg( "count", static_cast<std::int64_t>( window.ranges.size() ) )
                    .sortedBy( "window" ) );
      return false;
    }
    // The Window is a unit wide: storage's units are as wide as every Window
    // that shows their set, and without storage a unit is the Window's own
    // size — see docs/spec/xex.md.
    // A unit's state of the Window: the set's first, and the unit on from it.
    std::uint32_t const firstState =
        filled.has_value() ? window.firstStateOf( *filled, target.unitSets ).value_or( 0 ) : 0;

    // The state into the cell, as wide as the driver's `showAt` takes it,
    // and INITAD at the glue that hands it over.
    auto const show = [&]( std::uint32_t state )
    {
      if ( stateBytesOf( project, *project.driver->stream ) > 1 )
      {
        out.word( *loadUnit, state );
      }
      else
      {
        out.byte( *loadUnit, static_cast<std::uint8_t>( state ) );
      }
      out.word( INITAD, *loadMap );
    };

    std::optional<std::uint32_t> mapped;
    for ( Stored const& one : stored )
    {
      // Piece by piece where an image runs from one unit into the next.
      std::uint32_t position = target.filledPositionOf( one.at );
      std::span<std::uint8_t const> left = one.form;
      // A Pane's Section is its bytes as they are; a Payload is its stored form.
      bool const plain = one.where.has_value() && !build.storage().hasPayload( *one.where );
      std::uint32_t const extentBegin = plain ? sizes.initialisedExtentOf( *one.where ).begin : 0;
      while ( !left.empty() )
      {
        StorageAddress const at = target.filledAddressAt( position );
        if ( !mapped.has_value() || *mapped != at.bank.value )
        {
          show( firstState + at.bank.value );
          mapped = at.bank.value;
        }
        std::size_t const piece = std::min<std::size_t>( left.size(), target.filledUnitSize() - at.offset );
        std::uint32_t const landing = window.ranges.front().begin + at.offset;
        std::vector<Carried> carried;
        if ( plain )
        {
          carried.push_back(
              Carried{ .where = *one.where,
                       .offset = extentBegin + static_cast<std::uint32_t>( one.form.size() - left.size() ),
                       .at = landing,
                       .size = static_cast<std::uint32_t>( piece ) } );
        }
        out.segment( landing, left.subspan( 0, piece ), compressible( one.where ), std::move( carried ) );
        left = left.subspan( piece );
        position += static_cast<std::uint32_t>( piece );
      }
    }

    // Then each named state that holds bytes, shown as a unit is and written
    // at the Window's addresses — after every unit, so that the program
    // starts with the last of them shown, which is as undefined as any — see
    // docs/decisions/0256-a-named-state-is-filled-where-a-xex-can-show-it.md.
    std::optional<std::uint32_t> shown;
    for ( StateStored const& one : states )
    {
      if ( !shown.has_value() || *shown != one.state )
      {
        show( one.state );
        shown = one.state;
      }
      out.section( window.ranges.front().begin + one.offset,
                   one.form,
                   one.where,
                   sizes.initialisedExtentOf( one.where ).begin,
                   compressible( one.where ) );
    }
  }

  for ( Resident const* one : later )
  {
    out.section( one->address, one->content, one->where, one->address - layout.addressOf( one->where ) );
  }

  std::optional<std::uint32_t> const entry = startAddressOf( build, sink );
  if ( !entry.has_value() )
  {
    return false;
  }
  // Through the stub where there is one, so that the program's return reaches
  // the driver's `finalize` before it reaches the DOS — see
  // docs/decisions/0229-no-window-has-a-base.md.
  // Without one, through the cold start where the program takes one, which
  // brings the transformed Sections of the entry Phase before it is entered —
  // see docs/decisions/0236-a-xex-with-storage-brings-a-transformed-start-by-a-cold-start.md.
  std::optional<std::uint32_t> const stub = addressOfExported( symbols, RUN_NAME, sizes, layout );
  std::optional<std::uint32_t> const cold = addressOfExported( symbols, COLD_START_NAME, sizes, layout );
  out.word( RUNAD, stub.value_or( cold.value_or( *entry ) ) );
  return true;
}

/// What a block of a compressed file is carried as: the stored form of all
/// but its last `tail` bytes, which ends `margin` bytes past them and so
/// inside the block, and those bytes, written as they are after the decode.
struct Packed
{
  std::vector<std::uint8_t> form;
  std::uint32_t margin = 0;
  std::uint32_t tail = 0;
};

/// Below this a block's form, its cells and its INITAD cost what it saves.
constexpr std::size_t SMALLEST_COMPRESSED = 32;

/// The tail tried first: what a ZX0 form's margin comes to almost always, so
/// that a block is usually encoded once.
constexpr std::uint32_t FIRST_TAIL = 3;
constexpr int TAIL_TRIES = 4;

/// A block encoded so that its form ends inside it: the rest encoded, and the
/// tail lengthened to the margin of that rest until it covers it. Nothing
/// where the format cannot be decoded in place or the tail does not settle.
std::optional<Packed> packBlock( Format const& format, std::span<std::uint8_t const> bytes )
{
  std::uint32_t tail = FIRST_TAIL;
  for ( int tries = 0; tries < TAIL_TRIES && tail < bytes.size(); ++tries )
  {
    std::uint32_t const rest = static_cast<std::uint32_t>( bytes.size() ) - tail;
    std::vector<std::uint8_t> form = storedFormAtLoad( format.encode( bytes.first( rest ) ) );
    std::optional<std::uint32_t> const margin = format.margin != nullptr ? format.margin( form ) : std::nullopt;
    if ( !margin.has_value() )
    {
      return std::nullopt;
    }
    if ( *margin <= tail )
    {
      if ( form.size() > rest + *margin )
      {
        return std::nullopt;
      }
      return Packed{ .form = std::move( form ), .margin = *margin, .tail = tail };
    }
    tail = *margin;
  }
  return std::nullopt;
}

/// The ranges of `free` a block touches and what each part is to the
/// compression: written last, a candidate, or written as it is.
enum class Part : std::uint8_t
{
  LAST,
  CANDIDATE,
  AS_IT_IS,
};

bool inRanges( std::span<AddressRange const> ranges, std::uint32_t address )
{
  return std::ranges::any_of(
      ranges, [address]( AddressRange const& range ) { return address >= range.begin && address < range.end; } );
}

/// A piece of a run cut where it enters or leaves `nga.load` or the memory the
/// loader may write, each part said what it is.
std::vector<std::pair<Part, Piece>>
partsOf( Piece const& piece, std::span<AddressRange const> decoder, std::span<AddressRange const> writable )
{
  std::vector<std::uint32_t> cuts{ piece.at, piece.end() };
  for ( std::span<AddressRange const> const ranges : { decoder, writable } )
  {
    for ( AddressRange const& range : ranges )
    {
      for ( std::uint32_t const edge : { range.begin, range.end } )
      {
        if ( edge > piece.at && edge < piece.end() )
        {
          cuts.push_back( edge );
        }
      }
    }
  }
  std::ranges::sort( cuts );
  auto const [first, last] = std::ranges::unique( cuts );
  cuts.erase( first, last );

  std::vector<std::pair<Part, Piece>> parts;
  for ( std::size_t index = 0; index + 1 < cuts.size(); ++index )
  {
    std::uint32_t const begin = cuts[index];
    Part kind = Part::AS_IT_IS;
    if ( inRanges( decoder, begin ) )
    {
      kind = Part::LAST;
    }
    else if ( inRanges( writable, begin ) )
    {
      kind = Part::CANDIDATE;
    }
    auto const from = piece.bytes.begin() + ( begin - piece.at );
    auto const to = piece.bytes.begin() + ( cuts[index + 1] - piece.at );
    std::vector<Carried> carried = clipped( piece.carried, begin, cuts[index + 1] );
    if ( !parts.empty() && parts.back().first == kind )
    {
      parts.back().second.bytes.insert( parts.back().second.bytes.end(), from, to );
      parts.back().second.carried.insert( parts.back().second.carried.end(), carried.begin(), carried.end() );
      continue;
    }
    parts.emplace_back( kind,
                        Piece{ .at = begin,
                               .bytes = { from, to },
                               .compressible = true,
                               .carried = std::move( carried ),
                               .decodes = {} } );
  }
  for ( std::pair<Part, Piece>& part : parts )
  {
    if ( part.first == Part::CANDIDATE && part.second.bytes.size() < SMALLEST_COMPRESSED )
    {
      part.first = Part::AS_IT_IS;
    }
  }
  return parts;
}

/// The pieces of a run in the order they are written: by address and joined
/// where they touch, since nothing runs between them and the file's own rule
/// joins them — unless two of them write one byte, when the order they were
/// composed in is what the program meant.
std::vector<Piece> orderedRun( std::span<Piece const> run )
{
  std::vector<Piece> sorted{ run.begin(), run.end() };
  std::ranges::stable_sort( sorted, {}, &Piece::at );
  for ( std::size_t index = 1; index < sorted.size(); ++index )
  {
    if ( sorted[index].at < sorted[index - 1].end() )
    {
      return { run.begin(), run.end() };
    }
  }
  std::vector<Piece> joined;
  for ( Piece& piece : sorted )
  {
    if ( !joined.empty() && joined.back().compressible && piece.compressible && joined.back().end() == piece.at )
    {
      joined.back().bytes.insert( joined.back().bytes.end(), piece.bytes.begin(), piece.bytes.end() );
      joined.back().carried.insert( joined.back().carried.end(), piece.carried.begin(), piece.carried.end() );
      continue;
    }
    joined.push_back( std::move( piece ) );
  }
  return joined;
}

/// Encodes every candidate at once, each block alone and in no order, since
/// the encoder holds no state: the build's longest wait, shortened by every
/// processor the machine has.
std::vector<std::optional<Packed>> packAll( Format const& format, std::vector<Piece const*> const& blocks )
{
  std::vector<std::optional<Packed>> packed( blocks.size() );
  std::atomic<std::size_t> next{ 0 };
  auto const work = [&]
  {
    for ( std::size_t index = next++; index < blocks.size(); index = next++ )
    {
      packed[index] = packBlock( format, blocks[index]->bytes );
    }
  };
  std::size_t const workers =
      std::clamp<std::size_t>( std::thread::hardware_concurrency(), 1, std::max<std::size_t>( blocks.size(), 1 ) );
  std::vector<std::future<void>> running;
  running.reserve( workers );
  for ( std::size_t worker = 0; worker < workers; ++worker )
  {
    running.push_back( std::async( std::launch::async, work ) );
  }
  for ( std::future<void>& one : running )
  {
    one.get();
  }
  return packed;
}

/// The file compressed segment by segment: every block that saves bytes as
/// its form, the cells that say where the form stands and where it goes,
/// INITAD at the format's glue and then the block's tail; everything else as
/// it was. What of a run lies under `nga.load` is written once everything is
/// decoded, which is safe because the decoder keeps off the Windows and off
/// everything the loader calls, so nothing the loader runs before the end
/// needs those bytes. Nothing where the file has no decoder — see
/// docs/decisions/0259-a-xex-may-be-compressed-segment-by-segment.md.
std::optional<std::vector<Piece>> compressPlan( Patched const& build, Plan const& plan, Format const& format )
{
  GlobalSymbols const& symbols = build.symbols();
  Sizes const& sizes = build.sizes();
  Layout const& layout = build.layout();
  std::optional<ModuleIndex> const loadModule = build.project().loadModule;
  std::optional<std::uint32_t> const glueAt = addressOfExported( symbols, loadGlueName( format.name ), sizes, layout );
  std::optional<std::uint32_t> const fromAt = addressOfExported( symbols, UNPACK_FROM_NAME, sizes, layout );
  std::optional<std::uint32_t> const intoAt = addressOfExported( symbols, UNPACK_TO_NAME, sizes, layout );
  std::optional<std::size_t> const decoderAt = plan.decoderAt();
  if ( !decoderAt.has_value() || !loadModule.has_value() || !glueAt.has_value() || !fromAt.has_value() ||
       !intoAt.has_value() )
  {
    return std::nullopt;
  }
  ModuleIndex const load = *loadModule;
  std::uint32_t const glue = *glueAt;
  std::uint32_t const from = *fromAt;
  std::uint32_t const into = *intoAt;
  std::size_t const firstDecoded = *decoderAt;

  // What `nga.load` stands on, its cells and its zero page among it.
  std::vector<AddressRange> decoder;
  Module const& module = build.modules()[load.value];
  for ( std::uint32_t index = 0; index < module.sections().size(); ++index )
  {
    SectionRef const where{ .module = load, .section = SectionIndex{ index } };
    if ( layout.isPlaced( where ) && sizes.isKnown( where ) && sizes.sizeOfSection( where ) > 0 )
    {
      std::uint32_t const at = layout.addressOf( where );
      decoder.push_back( AddressRange{ .begin = at, .end = at + sizes.sizeOfSection( where ) } );
    }
  }
  std::vector<AddressRange> const writable = writableAtLoad( build );

  // Every run cut into its parts, the candidates encoded together after.
  std::vector<Piece> const& pieces = plan.pieces();
  std::vector<std::pair<Part, Piece>> parts;
  parts.reserve( pieces.size() );
  std::vector<Piece> last;
  for ( std::size_t index = 0; index < firstDecoded; ++index )
  {
    parts.emplace_back( Part::AS_IT_IS, pieces[index] );
  }
  std::size_t begin = firstDecoded;
  while ( begin < pieces.size() )
  {
    std::size_t end = begin;
    while ( end < pieces.size() && !pieces[end].callsInit() )
    {
      ++end;
    }
    for ( Piece& piece : orderedRun( std::span{ pieces }.subspan( begin, end - begin ) ) )
    {
      if ( !piece.compressible )
      {
        parts.emplace_back( Part::AS_IT_IS, std::move( piece ) );
        continue;
      }
      for ( std::pair<Part, Piece>& part : partsOf( piece, decoder, writable ) )
      {
        if ( part.first == Part::LAST )
        {
          last.push_back( std::move( part.second ) );
        }
        else
        {
          parts.push_back( std::move( part ) );
        }
      }
    }
    if ( end < pieces.size() )
    {
      parts.emplace_back( Part::AS_IT_IS, pieces[end] );
    }
    begin = end + 1;
  }

  std::vector<Piece const*> blocks;
  for ( std::pair<Part, Piece> const& part : parts )
  {
    if ( part.first == Part::CANDIDATE )
    {
      blocks.push_back( &part.second );
    }
  }
  std::vector<std::optional<Packed>> const packed = packAll( format, blocks );

  std::vector<Piece> out;
  std::size_t block = 0;
  for ( std::pair<Part, Piece>& part : parts )
  {
    if ( part.first != Part::CANDIDATE )
    {
      out.push_back( std::move( part.second ) );
      continue;
    }
    std::optional<Packed> const& one = packed[block++];
    Piece& piece = part.second;
    // The form, the cells, INITAD and the tail, against the block as it is:
    // a header for each segment, and the cells' one shared.
    std::size_t const cost = one.has_value() ? 4 + one->form.size() + 8 + 6 + ( one->tail > 0 ? 4 + one->tail : 0 ) : 0;
    if ( !one.has_value() || cost >= 4 + piece.bytes.size() )
    {
      out.push_back( std::move( piece ) );
      continue;
    }
    std::uint32_t const rest = static_cast<std::uint32_t>( piece.bytes.size() ) - one->tail;
    std::uint32_t const formAt = piece.at + rest + one->margin - static_cast<std::uint32_t>( one->form.size() );
    out.push_back( Piece{ .at = formAt, .bytes = one->form, .compressible = false, .carried = {}, .decodes = {} } );
    out.push_back( wordPiece( from, formAt ) );
    out.push_back( wordPiece( into, piece.at ) );
    Piece call = wordPiece( INITAD, glue );
    call.decodes = clipped( piece.carried, piece.at, piece.at + rest );
    out.push_back( std::move( call ) );
    if ( one->tail > 0 )
    {
      out.push_back( Piece{ .at = piece.at + rest,
                            .bytes = { piece.bytes.begin() + rest, piece.bytes.end() },
                            .compressible = false,
                            .carried = clipped( piece.carried, piece.at + rest, piece.end() ),
                            .decodes = {} } );
    }
  }
  for ( Piece& piece : last )
  {
    out.push_back( std::move( piece ) );
  }
  return out;
}

/// Where the program's own bytes stand in base memory once the file has
/// loaded, byte by byte, and which of them are sites: memory a list may be
/// borrowed from, since the program holds those bytes again by the end. And
/// what nothing the file writes stands on, which a list may stand in too.
/// Nothing where a byte stands in a Window or is what runs while the file
/// loads needs: `nga.load`, `nga.choose` and the load's own code — see
/// docs/decisions/0302-a-document-may-be-chosen-while-a-xex-loads.md.
struct Lendable
{
  std::vector<std::optional<std::uint8_t>> image;
  std::vector<bool> site;
  std::vector<bool> kept;
};

Lendable lendableOf( Patched const& build )
{
  constexpr std::uint32_t memory = 0x10000;
  Project const& project = build.project();
  Layout const& layout = build.layout();
  Sizes const& sizes = build.sizes();
  std::vector<std::optional<std::uint8_t>> image( memory );
  std::vector<AddressRange> needed;
  for ( std::uint32_t module = 0; module < build.modules().size(); ++module )
  {
    Module const& one = build.modules()[module];
    for ( std::uint32_t index = 0; index < one.sections().size(); ++index )
    {
      SectionRef const where{ .module = ModuleIndex{ module }, .section = SectionIndex{ index } };
      Section const& section = one.sections()[index];
      if ( !layout.isPlaced( where ) || !sizes.isKnown( where ) || section.inMedium() )
      {
        continue;
      }
      std::uint32_t const at = layout.addressOf( where );
      if ( ModuleIndex{ module } == project.loadModule || ModuleIndex{ module } == project.chooseModule ||
           isLoadCode( build, where ) )
      {
        needed.push_back( AddressRange{ .begin = at, .end = at + sizes.sizeOfSection( where ) } );
        continue;
      }
      if ( !section.emitsBytes() || !heldAcrossOf( build, where ).includes( project.phases.entry ) ||
           build.storage().isHiddenAtLoad( where ) )
      {
        continue;
      }
      InitialisedExtent const extent = sizes.initialisedExtentOf( where );
      std::span<std::uint8_t const> const content = contentOf( build.bytes(), where, extent );
      for ( std::size_t offset = 0; offset < content.size() && at + extent.begin + offset < memory; ++offset )
      {
        image[at + extent.begin + offset] = content[offset];
      }
    }
  }
  std::vector<bool> kept( memory, false );
  for ( AddressRange const& range : keptWhileLoading( build, build.storage() ) )
  {
    for ( std::uint32_t at = range.begin; at < range.end && at < memory; ++at )
    {
      kept[at] = true;
    }
  }
  std::vector<bool> sites( memory, false );
  for ( Storage::ChoiceSite const& site : build.storage().choiceSites() )
  {
    if ( std::uint32_t const at = layout.addressOf( site.where ) + site.offset; at < memory )
    {
      sites[at] = true;
    }
  }
  for ( Window const& window : build.target().windows )
  {
    needed.insert( needed.end(), window.ranges.begin(), window.ranges.end() );
  }
  // The zero page, the stack, and the vectors the file writes.
  needed.push_back( AddressRange{ .begin = 0, .end = 0x200 } );
  needed.push_back( AddressRange{ .begin = RUNAD, .end = INITAD + 2 } );
  for ( AddressRange const& range : needed )
  {
    for ( std::uint32_t at = range.begin; at < range.end && at < memory; ++at )
    {
      image[at].reset();
      kept[at] = false;
    }
  }
  return Lendable{ .image = std::move( image ), .site = std::move( sites ), .kept = std::move( kept ) };
}

/// The file with what changes it for a Choice: before every `INITAD`, and
/// once after the last segment, a call of `ngaPatch` with the entries of the
/// sites written since the last call where they stand then — in a unit while
/// it is shown, in a copy before the move takes it under a ROM, in a Section
/// once its decode has returned. Nothing the load runs meets a site not
/// changed yet, since it runs only through `INITAD`. The load's own code is
/// changed again before every `init` after the first, since only once the
/// first has returned is anything chosen.
///
/// The entries of a call are written just before it, into memory borrowed from
/// the program: where nothing has been written yet, which the program's bytes
/// are written over later, or else where the program's bytes are already, no
/// site among them and nothing of the call's own run, which are written again
/// just after it. Where no stretch holds them all they are cut into several
/// calls, each in a stretch of its own. A site never written as it is is
/// `NGA6247`, entries with nowhere to borrow from `NGA5273`, and nothing is
/// written where either was reported — see
/// docs/decisions/0302-a-document-may-be-chosen-while-a-xex-loads.md.
std::optional<std::vector<Piece>>
withChoices( Patched const& build, std::vector<Piece> pieces, diag::DiagnosticSink& sink )
{
  Project const& project = build.project();
  std::span<Storage::ChoiceSite const> const sites = build.storage().choiceSites();
  if ( sites.empty() )
  {
    return pieces;
  }
  std::optional<std::uint32_t> const patchAt =
      addressOfExported( build.symbols(), PATCH_NAME, build.sizes(), build.layout() );
  std::optional<std::uint32_t> const fromAt =
      addressOfExported( build.symbols(), PATCH_FROM_NAME, build.sizes(), build.layout() );
  std::optional<std::uint32_t> const toAt =
      addressOfExported( build.symbols(), PATCH_TO_NAME, build.sizes(), build.layout() );
  if ( !patchAt.has_value() || !fromAt.has_value() || !toAt.has_value() )
  {
    return std::nullopt;
  }

  std::map<std::tuple<std::uint32_t, std::uint32_t>, std::vector<std::size_t>> bySection;
  for ( std::size_t index = 0; index < sites.size(); ++index )
  {
    bySection[{ sites[index].where.module.value, sites[index].where.section.value }].push_back( index );
  }

  // What each `init` after the first is called at.
  std::vector<std::uint32_t> laterInits;
  diag::DiagnosticSink quiet{ sink.policy() };
  for ( std::size_t index = 1; index < project.loadInits.size(); ++index )
  {
    if ( std::optional<std::uint32_t> const address = loadInitAddress( build, project.loadInits[index], quiet );
         address.has_value() )
    {
      laterInits.push_back( *address );
    }
  }
  auto const callsLaterInit = [&laterInits]( Piece const& piece )
  {
    if ( piece.at != INITAD || piece.bytes.size() != 2 )
    {
      return false;
    }
    std::uint32_t const called = piece.bytes[0] | static_cast<std::uint32_t>( piece.bytes[1] << 8U );
    return std::ranges::find( laterInits, called ) != laterInits.end();
  };

  // What each byte of base memory holds as the file goes, where a segment
  // put it, and whether one has.
  Lendable const lendable = lendableOf( build );
  std::vector<bool> written( lendable.image.size(), false );
  std::vector<std::optional<std::uint8_t>> holds( lendable.image.size() );
  auto const wrote = [&]( Piece const& piece, bool lands )
  {
    for ( std::uint32_t at = piece.at; at < piece.end() && at < holds.size(); ++at )
    {
      holds[at] = piece.bytes[at - piece.at];
      written[at] = written[at] || lands;
    }
  };
  auto const decoded = [&]( Carried const& carried )
  {
    for ( std::uint32_t at = carried.at; at < carried.at + carried.size && at < holds.size(); ++at )
    {
      holds[at] = lendable.image[at];
      written[at] = true;
    }
  };
  // Every stretch of the program's own bytes, or of what nothing writes, that
  // `free` says may be borrowed and that holds an entry at least, the longest
  // first.
  std::uint32_t const entrySize = 2 + static_cast<std::uint32_t>( project.choices.size() ) - 1;
  auto const stretches = [&]( auto const& free )
  {
    std::vector<AddressRange> found;
    std::uint32_t begin = 0;
    for ( std::uint32_t at = 0; at <= lendable.image.size(); ++at )
    {
      if ( at < lendable.image.size() && ( lendable.image[at].has_value() || lendable.kept[at] ) && free( at ) )
      {
        continue;
      }
      if ( at - begin >= entrySize )
      {
        found.push_back( AddressRange{ .begin = begin, .end = at } );
      }
      begin = at + 1;
    }
    std::ranges::stable_sort( found, std::ranges::greater{}, &AddressRange::size );
    return found;
  };

  struct Pending
  {
    std::uint32_t address = 0;
    std::size_t site = 0;
  };

  std::vector<Pending> pending;
  std::vector<Pending> loadCode;
  std::vector<std::size_t> reached( sites.size(), 0 );
  std::vector<Piece> out;
  bool refused = false;
  auto const reach = [&]( Carried const& carried )
  {
    auto const found = bySection.find( { carried.where.module.value, carried.where.section.value } );
    if ( found == bySection.end() )
    {
      return;
    }
    for ( std::size_t const site : found->second )
    {
      std::uint32_t const offset = sites[site].offset;
      if ( offset >= carried.offset && offset < carried.offset + carried.size )
      {
        Pending const one{ .address = carried.at + ( offset - carried.offset ), .site = site };
        ( isLoadCode( build, carried.where ) ? loadCode : pending ).push_back( one );
        ++reached[site];
      }
    }
  };
  auto const flush = [&]
  {
    if ( pending.empty() )
    {
      return;
    }
    std::vector<Pending> left = std::move( pending );
    pending.clear();
    // Memory nothing has been written to yet first, where a site is as good
    // as any byte since its own segment comes later; then memory that holds
    // what the program holds there by the end, which is written again after
    // the call and so ends as it was, whatever it was for meanwhile — but no
    // site, which may have been changed already.
    std::vector<std::pair<AddressRange, bool>> lent;
    for ( AddressRange const& range : stretches( [&written]( std::uint32_t address ) { return !written[address]; } ) )
    {
      lent.emplace_back( range, false );
    }
    for ( AddressRange const& range : stretches(
              [&]( std::uint32_t address )
              {
                return lendable.image[address].has_value() && !lendable.site[address] &&
                       holds[address] == lendable.image[address];
              } ) )
    {
      lent.emplace_back( range, true );
    }
    std::vector<std::pair<AddressRange, bool>> used;
    for ( auto const& [range, again] : lent )
    {
      if ( left.empty() )
      {
        break;
      }
      // A stretch written already may overlap one that is not, which the
      // entries before took.
      if ( std::ranges::any_of( used,
                                [&range]( auto const& one )
                                { return range.begin < one.first.end && one.first.begin < range.end; } ) )
      {
        continue;
      }
      std::size_t const fits = std::min<std::size_t>( left.size(), range.size() / entrySize );
      std::vector<std::uint8_t> entries;
      for ( std::size_t index = 0; index < fits; ++index )
      {
        Pending const& one = left[index];
        entries.push_back( static_cast<std::uint8_t>( one.address & 0xFFU ) );
        entries.push_back( static_cast<std::uint8_t>( ( one.address >> 8U ) & 0xFFU ) );
        std::vector<std::uint8_t> const& values = sites[one.site].values;
        entries.insert( entries.end(), values.begin() + 1, values.end() );
      }
      left.erase( left.begin(), left.begin() + static_cast<std::ptrdiff_t>( fits ) );
      std::uint32_t const at = range.begin;
      auto const size = static_cast<std::uint32_t>( entries.size() );
      used.emplace_back( AddressRange{ .begin = at, .end = at + size }, again );
      Piece list{ .at = at, .bytes = std::move( entries ), .compressible = false, .carried = {}, .decodes = {} };
      wrote( list, false );
      out.push_back( std::move( list ) );
      out.push_back( wordPiece( *fromAt, at ) );
      out.push_back( wordPiece( *toAt, at + size ) );
      out.push_back( wordPiece( INITAD, *patchAt ) );
      if ( again )
      {
        Piece restored{ .at = at, .bytes = {}, .compressible = false, .carried = {}, .decodes = {} };
        for ( std::uint32_t address = at; address < at + size; ++address )
        {
          restored.bytes.push_back( lendable.image[address].value_or( 0 ) );
        }
        wrote( restored, true );
        out.push_back( std::move( restored ) );
      }
    }
    if ( !left.empty() )
    {
      sink.add( diag::diagnostic( diag::DiagnosticId::CHOOSE_NO_ROOM )
                    .arg( "size", static_cast<std::int64_t>( left.size() * entrySize ) )
                    .sortedBy( "choose" ) );
      refused = true;
    }
  };
  for ( Piece& piece : pieces )
  {
    if ( callsLaterInit( piece ) )
    {
      pending.insert( pending.end(), loadCode.begin(), loadCode.end() );
    }
    if ( piece.callsInit() )
    {
      flush();
    }
    // A byte written over is no longer the site it was.
    auto const over = [&piece]( Pending const& one ) { return one.address >= piece.at && one.address < piece.end(); };
    std::erase_if( pending, over );
    std::erase_if( loadCode, over );
    std::vector<Carried> const carried = std::move( piece.carried );
    std::vector<Carried> const decodes = std::move( piece.decodes );
    wrote( piece, true );
    out.push_back( std::move( piece ) );
    for ( Carried const& one : carried )
    {
      reach( one );
    }
    for ( Carried const& one : decodes )
    {
      decoded( one );
      reach( one );
    }
  }
  flush();

  std::optional<SectionRef> reported;
  for ( std::size_t index = 0; index < sites.size(); ++index )
  {
    Storage::ChoiceSite const& site = sites[index];
    if ( reached[index] > 0 || reported == site.where )
    {
      continue;
    }
    Module const& module = build.modules()[site.where.module.value];
    Section const& section = module.sectionAt( site.where.section );
    auto const choice = static_cast<std::size_t>( std::ranges::find_if( site.values.begin() + 1,
                                                                        site.values.end(),
                                                                        [&site]( std::uint8_t value )
                                                                        { return value != site.values.front(); } ) -
                                                  site.values.begin() );
    sink.add( diag::diagnostic( diag::DiagnosticId::CHOICE_NOT_REACHED )
                  .at( section.span().begin, section.span().length )
                  .arg( "document", project.choices[std::min( choice, project.choices.size() - 1 )].document )
                  .arg( "section", module.displayNameOf( site.where.section ) )
                  .arg( "offset", static_cast<std::int64_t>( site.offset ) ) );
    reported = site.where;
    refused = true;
  }
  if ( refused )
  {
    return std::nullopt;
  }
  return out;
}

} // namespace

XexFile emitXex( Patched const& build, diag::DiagnosticSink& sink )
{
  Plan plain;
  if ( !composeXex( build, plain, false, sink ) )
  {
    return {};
  }
  std::optional<std::vector<Piece>> const chosen = withChoices( build, plain.pieces(), sink );
  if ( !chosen.has_value() )
  {
    return {};
  }
  std::vector<std::uint8_t> file = fileOf( *chosen );
  std::optional<std::string> const& format = build.project().compress;
  if ( !format.has_value() )
  {
    return XexFile{ .bytes = std::move( file ) };
  }

  // The same file in the order compression wants, which reports nothing the
  // plain one did not; and written as the plain one where compressing it
  // saves less than the decoder costs.
  diag::DiagnosticSink quiet{ sink.policy() };
  Plan reordered;
  if ( !composeXex( build, reordered, true, quiet ) || quiet.hasErrors() )
  {
    return XexFile{ .bytes = std::move( file ) };
  }
  std::optional<std::vector<Piece>> compressed = compressPlan( build, reordered, *formatNamed( *format ) );
  if ( compressed.has_value() )
  {
    compressed = withChoices( build, std::move( *compressed ), quiet );
  }
  if ( compressed.has_value() && !quiet.hasErrors() )
  {
    std::vector<std::uint8_t> smaller = fileOf( *compressed );
    if ( smaller.size() < file.size() )
    {
      return XexFile{ .bytes = std::move( smaller ) };
    }
  }
  return XexFile{ .bytes = std::move( file ) };
}

} // namespace nga::model
