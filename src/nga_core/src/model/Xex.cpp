#include "nga/model/Emit.hpp"

#include "nga/model/Transition.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace nga::model
{

namespace
{

/// Appends segments in the shape a DOS loader reads: start and end, both two
/// bytes little endian and the end inclusive, then exactly the bytes between.
///
/// A segment that begins where the one before it ended is not a segment of
/// its own: the bytes are appended to the one before and its end moved,
/// which is the same load four bytes shorter. A Proc is a Section, and a
/// Module of Procs would otherwise pay a header per routine.
class XexWriter
{
public:
  XexWriter()
  {
    mBytes.push_back( 0xFF );
    mBytes.push_back( 0xFF );
  }

  void segment( std::uint32_t start, std::span<std::uint8_t const> data )
  {
    std::uint32_t const end = start + static_cast<std::uint32_t>( data.size() ) - 1;
    if ( mOpen.has_value() && mOpen->end + 1 == start )
    {
      mOpen->end = end;
      writeAddress( mOpen->endAt, end );
      mBytes.insert( mBytes.end(), data.begin(), data.end() );
      return;
    }
    address( start );
    mOpen = Open{ .end = end, .endAt = mBytes.size() };
    address( end );
    mBytes.insert( mBytes.end(), data.begin(), data.end() );
  }

  void byte( std::uint32_t at, std::uint8_t value )
  {
    std::array<std::uint8_t, 1> const one{ value };
    segment( at, one );
  }

  void word( std::uint32_t at, std::uint32_t value )
  {
    std::array<std::uint8_t, 2> const two{ static_cast<std::uint8_t>( value & 0xFF ),
                                           static_cast<std::uint8_t>( ( value >> 8 ) & 0xFF ) };
    segment( at, two );
  }

  [[nodiscard]] std::vector<std::uint8_t> take() &&
  {
    return std::move( mBytes );
  }

private:
  /// The segment being written: where it ends, and where in the file its
  /// end address stands, so that a contiguous one can move it.
  struct Open
  {
    std::uint32_t end = 0;
    std::size_t endAt = 0;
  };

  void address( std::uint32_t value )
  {
    mBytes.push_back( static_cast<std::uint8_t>( value & 0xFF ) );
    mBytes.push_back( static_cast<std::uint8_t>( ( value >> 8 ) & 0xFF ) );
  }

  void writeAddress( std::size_t at, std::uint32_t value )
  {
    mBytes[at] = static_cast<std::uint8_t>( value & 0xFF );
    mBytes[at + 1] = static_cast<std::uint8_t>( ( value >> 8 ) & 0xFF );
  }

  std::vector<std::uint8_t> mBytes;
  std::optional<Open> mOpen;
};

/// Something with a place to wait — a Payload or a Frame — and where.
struct Stored
{
  StorageAddress at;
  std::span<std::uint8_t const> form;
};

/// A DOS calls the address written here after the segment that wrote it,
/// which is how a file runs the driver while it is loaded.
constexpr std::uint32_t INITAD = 0x02E2;

/// The runtime address of a Label the whole program can see, or nothing.
std::optional<std::uint32_t>
addressOfExported( GlobalSymbols const& symbols, std::string_view name, Sizes const& sizes, Layout const& layout )
{
  std::optional<SymbolRef> const where = symbols.find( name );
  return where.has_value() ? addressOfLabel( symbols, *where, sizes, layout ) : std::nullopt;
}

/// The bytes Patch produced for a Section, no more than its size.
/// The bytes Patch produced for a Section's initialised extent, clamped to
/// what Patch produced: a Section shorter than its size is a defect of an
/// earlier Step and not of the writer.
std::span<std::uint8_t const> contentOf( Bytes const& bytes, SectionRef where, InitialisedExtent extent )
{
  std::span<std::uint8_t const> const content = bytes.of( where );
  std::size_t const begin = std::min<std::size_t>( extent.begin, content.size() );
  return content.subspan( begin, std::min<std::size_t>( extent.size(), content.size() - begin ) );
}

} // namespace

XexFile emitXex( Patched const& build, diag::DiagnosticSink& sink )
{
  Project const& project = build.project();
  GlobalSymbols const& symbols = build.symbols();
  Sizes const& sizes = build.sizes();
  Storage const& storage = build.storage();
  Layout const& layout = build.layout();
  Bytes const& bytes = build.bytes();
  Target const& target = build.target();
  std::span<Module const> const modules = symbols.modules();

  // Every Payload with a place to wait, by Bank and then by offset: one switch
  // per Bank rather than one per Payload.
  std::vector<Stored> stored;
  for ( std::uint32_t module = 0; module < modules.size(); ++module )
  {
    for ( std::uint32_t index = 0; index < modules[module].sections().size(); ++index )
    {
      SectionRef const where{ .module = ModuleIndex{ module }, .section = SectionIndex{ index } };
      // The Payload's size is Storage's: what waits in a Bank is the stored
      // form, and the Section's own size is what it occupies once loaded.
      if ( !storage.hasPayload( where ) || !storage.isPlaced( where ) || storage.sizeOf( where ) == 0 )
      {
        continue;
      }
      stored.push_back( Stored{ .at = storage.addressOf( where ), .form = storage.formOf( where ) } );
    }
  }
  // And every Pane's Section with bytes, which the loader writes into the
  // Pane's Bank at its offset within the Window, once — see
  // docs/decisions/0054-panes.md.
  for ( Storage::PaneImage const& image : storage.paneImages() )
  {
    stored.push_back(
        Stored{ .at = image.at, .form = contentOf( bytes, image.where, sizes.initialisedExtentOf( image.where ) ) } );
  }
  // And every Frame, which waits as a Payload does and is read by the
  // routine alone.
  for ( std::uint32_t index = 0; index < storage.frameCount(); ++index )
  {
    FrameIndex const frame{ index };
    if ( storage.isFramePlaced( frame ) && !storage.frameBytesOf( frame ).empty() )
    {
      stored.push_back( Stored{ .at = storage.frameAddressOf( frame ), .form = storage.frameBytesOf( frame ) } );
    }
  }
  std::ranges::stable_sort(
      stored,
      []( Stored const& a, Stored const& b )
      { return a.at.bank.value != b.at.bank.value ? a.at.bank.value < b.at.bank.value : a.at.offset < b.at.offset; } );

  XexWriter out;

  // Present when the program starts: everything the entry Phase needs that
  // emits bytes, in Project order. Before the units, because filling one
  // runs the driver, which is among them; a Section whose runtime address
  // is in the window is base memory, and stays so once the driver shows the
  // base again.
  Phase const& entryPhase = project.phases.phases[project.phases.entry.value];
  std::vector<bool> present( modules.size(), false );
  for ( ModuleIndex const module : entryPhase.needs )
  {
    present[module.value] = true;
  }
  for ( std::uint32_t module = 0; module < modules.size(); ++module )
  {
    if ( !present[module] )
    {
      continue;
    }
    Module const& one = modules[module];
    for ( std::uint32_t index = 0; index < one.sections().size(); ++index )
    {
      SectionRef const where{ .module = ModuleIndex{ module }, .section = SectionIndex{ index } };
      // A Pane's Section is in its Bank and not in base memory.
      if ( !layout.isPlaced( where ) || !sizes.isKnown( where ) || sizes.sizeOfSection( where ) == 0 ||
           !one.sections()[index].emitsBytes() || one.sections()[index].pane().has_value() )
      {
        continue;
      }
      // The initialised extent and not the Section: reserved space at either
      // end is not in the file, because a load writing it would write nothing
      // the program may rely on.
      InitialisedExtent const extent = sizes.initialisedExtentOf( where );
      out.segment( layout.addressIn( where, project.phases.entry ) + extent.begin, contentOf( bytes, where, extent ) );
    }
  }

  if ( !stored.empty() )
  {
    // The units are filled through the driver: a byte naming the unit's
    // state of the Window into the cell the dispatcher keeps for it, INITAD
    // at the glue that hands the byte to `showAt`, then the unit's images
    // into the window; and once through the Proc that shows the base
    // again, so that the window is base memory when the program starts.
    // Found here, when the Container that needs them is written.
    std::optional<std::uint32_t> const loadUnit = addressOfExported( symbols, LOAD_UNIT_NAME, sizes, layout );
    std::optional<std::uint32_t> const loadMap = addressOfExported( symbols, LOAD_MAP_NAME, sizes, layout );
    std::optional<std::uint32_t> const restore = addressOfExported( symbols, RESTORE_NAME, sizes, layout );
    if ( !project.driver.has_value() || !restore.has_value() )
    {
      sink.add( diag::diagnostic( diag::DiagnosticId::XEX_WITHOUT_DRIVER ).sortedBy( "driver" ) );
      return {};
    }
    if ( !project.driver->stream.has_value() || !target.storageUnits.has_value() || !loadUnit.has_value() ||
         !loadMap.has_value() )
    {
      sink.add( diag::diagnostic( diag::DiagnosticId::XEX_NEEDS_WINDOW ).sortedBy( "window" ) );
      return {};
    }
    Window const& window = target.windows[project.driver->stream->value];
    if ( window.ranges.size() != 1 )
    {
      sink.add( diag::diagnostic( diag::DiagnosticId::XEX_WINDOW_NOT_ONE_RANGE )
                    .arg( "name", window.name )
                    .arg( "count", static_cast<std::int64_t>( window.ranges.size() ) )
                    .sortedBy( "window" ) );
      return {};
    }
    if ( window.size() != target.unitSize )
    {
      sink.add( diag::diagnostic( diag::DiagnosticId::XEX_UNIT_NOT_WINDOW )
                    .arg( "unit", target.unitSize )
                    .arg( "name", window.name )
                    .arg( "window", window.size() )
                    .sortedBy( "window" ) );
      return {};
    }
    // A unit's state of the Window: the set's first, and the unit on from it.
    std::uint32_t const firstState = window.firstStateOf( *target.storageUnits, target.unitSets ).value_or( 0 );

    std::optional<std::uint32_t> mapped;
    for ( Stored const& one : stored )
    {
      // Piece by piece where an image runs from one unit into the next.
      std::uint32_t position = target.positionOf( one.at );
      std::span<std::uint8_t const> left = one.form;
      while ( !left.empty() )
      {
        StorageAddress const at = target.addressAt( position );
        if ( !mapped.has_value() || *mapped != at.bank.value )
        {
          out.byte( *loadUnit, static_cast<std::uint8_t>( firstState + at.bank.value ) );
          out.word( INITAD, *loadMap );
          mapped = at.bank.value;
        }
        std::size_t const piece = std::min<std::size_t>( left.size(), target.unitSize - at.offset );
        out.segment( window.ranges.front().begin + at.offset, left.subspan( 0, piece ) );
        left = left.subspan( piece );
        position += static_cast<std::uint32_t>( piece );
      }
    }
    out.word( INITAD, *restore );
  }

  // When some edge enters the entry Phase, Patch has resolved its entry for a
  // table and reported whatever was wrong with it; saying it again here would
  // be the same finding twice.
  diag::SeverityPolicy quietPolicy;
  diag::DiagnosticSink quiet{ quietPolicy };
  bool const alreadyReported = isEnteredByAnEdge( project.phases, project.phases.entry );
  std::optional<std::uint32_t> const run =
      entryAddressOf( build, project.phases.entry, alreadyReported ? quiet : sink );
  if ( !run.has_value() )
  {
    return {};
  }
  std::array<std::uint8_t, 2> const runAddress{ static_cast<std::uint8_t>( *run & 0xFF ),
                                                static_cast<std::uint8_t>( ( *run >> 8 ) & 0xFF ) };
  out.segment( 0x02E0, runAddress );

  return XexFile{ .bytes = std::move( out ).take() };
}

} // namespace nga::model
