#include "nga/model/Emit.hpp"

#include "nga/model/Segments.hpp"
#include "nga/model/Transition.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <optional>
#include <span>
#include <utility>
#include <vector>

namespace nga::model
{

XexFile emitXex( Patched const& build, diag::DiagnosticSink& sink )
{
  Project const& project = build.project();
  GlobalSymbols const& symbols = build.symbols();
  Sizes const& sizes = build.sizes();
  Layout const& layout = build.layout();
  Target const& target = build.target();

  std::vector<Stored> const stored = storedImages( build );

  SegmentWriter out;

  // Present when the program starts: everything the entry Phase needs that
  // emits bytes, in Project order. Before the units, because filling one
  // runs the driver, which is among them; a Section whose runtime address
  // is in the window is base memory, and stays so once the driver shows the
  // base again.
  writeResidentSegments( build, out, {} );

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
  out.word( RUNAD, *run );

  // A `.xex` begins with $FF $FF and is otherwise the segments.
  std::vector<std::uint8_t> file{ 0xFF, 0xFF };
  std::vector<std::uint8_t> const segments = std::move( out ).take();
  file.insert( file.end(), segments.begin(), segments.end() );
  return XexFile{ .bytes = std::move( file ) };
}

} // namespace nga::model
