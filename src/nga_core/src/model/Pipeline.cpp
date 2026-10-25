#include "nga/model/Pipeline.hpp"

#include "nga/model/Assemble.hpp"
#include "nga/model/Choose.hpp"
#include "nga/model/Emit.hpp"
#include "nga/model/Expand.hpp"
#include "nga/model/Load.hpp"
#include "nga/model/Merge.hpp"
#include "nga/model/Patch.hpp"
#include "nga/model/Place.hpp"
#include "nga/model/Prune.hpp"
#include "nga/model/ReadOnly.hpp"
#include "nga/model/Size.hpp"
#include "nga/model/Storage.hpp"
#include "nga/model/Suppressions.hpp"
#include "nga/model/Trace.hpp"
#include "nga/model/Transform.hpp"
#include "nga/model/TypeCheck.hpp"
#include "nga/model/VerifyLayout.hpp"

#include <spdlog/spdlog.h>

#include <chrono>
#include <span>
#include <string_view>

namespace nga::model
{

namespace
{

/// How long each Step took, said at debug level as it ends: what `--verbose`
/// shows of where a build spends its time. No Step reads it, so a build is
/// the same however long it took.
class StepClock
{
public:
  void ended( std::string_view step )
  {
    auto const now = std::chrono::steady_clock::now();
    spdlog::debug( "{}: {:.1f} ms", step, std::chrono::duration<double, std::milli>( now - mLast ).count() );
    mLast = now;
  }

private:
  std::chrono::steady_clock::time_point mLast = std::chrono::steady_clock::now();
};

/// Merge through Emit over the assembled Modules. Errors stop the build at the
/// end of the Step that raised them, so that a run reports everything that
/// Step found rather than the first thing.
void buildFrom( diag::SourceManager& sources,
                Project const& project,
                std::span<Project const> later,
                std::span<Module> modules,
                BuildOptions const& options,
                diag::DiagnosticSink& sink,
                std::function<void( Patched const&, Emitted const& )> const& built,
                std::function<void( Merged const& )> const& observed,
                StepClock& clock )
{
  GlobalSymbols const symbols = merge( modules, project, sink );

  // Merge's second result. A table cannot be built while a Module is
  // assembled, and Size needs one before it runs, which is what fixes this
  // position exactly — see docs/decisions/0013-charset-declaration.md.
  Charsets const charsets = resolveCharsets( sources, symbols, sink );
  clock.ended( "Merge" );

  // What every Step after Merge is handed: the results produced so far, and
  // nothing of the ones that do not exist yet — see
  // docs/decisions/0032-a-step-is-handed-the-build-so-far.md. Shown before
  // Expand, which empties the branches a condition excluded, and before the
  // gate, since a build that stops here still knows its names.
  Merged const merged{ sources, project, symbols, charsets };
  if ( observed )
  {
    observed( merged );
  }

  // What `.if` decides and what a macro use expands to, before anything walks
  // a Chunk: a Reference in a branch the build excluded would otherwise hold
  // its Section against Prune, and the type check would report on code that
  // is not there — see docs/decisions/0049-recursive-macros.md. The gate
  // stands after it so that a use naming no macro is reported in the same
  // run as a duplicate export.
  expand( sources, symbols, charsets, project, modules, sink );
  clock.ended( "Expand" );
  if ( sink.hasErrors() )
  {
    return;
  }

  // What `.root` hands the hardware is a Root from here on — see
  // docs/decisions/0061-root-at-the-taking.md.
  markRoots( sources, symbols, modules, sink );

  // What the Roots reach. Before the type check, so that a Reference from a
  // Section nothing reaches keeps nothing out from under a ROM; the rules are
  // still applied to every Chunk — see docs/decisions/0033-prune.md.
  Reachable const reachable = prune( merged, sink );
  Pruned const pruned{ merged, reachable };
  clock.ended( "Prune" );

  // Which Temporaries are never live at once, over the same References; what
  // Place reads to let them share an address — see
  // docs/decisions/0034-trace.md.
  auto const [interference, holds] = trace( pruned, sink );
  Traced const traced{ pruned, interference, holds };
  clock.ended( "Trace" );

  // Which Sections nothing writes, and which of those therefore stand in a
  // `rom` Region: after Trace, the last Step that reads what a Chunk does to
  // the memory it names, and before Size, since what it needs of a Section is
  // whether any Chunk of it emits — see
  // docs/decisions/0215-a-cartridge-is-rom-and-a-section-stands-in-it-when-nothing-writes-it.md.
  ReadOnly const readOnly = readOnlySections( pruned, sink );
  clock.ended( "ReadOnly" );

  Referrers const referrers = checkTypes( pruned, sink );
  clock.ended( "the type check" );
  Sizes const sizes = computeSizes( pruned, sink );
  clock.ended( "Size" );
  if ( sink.hasErrors() )
  {
    return;
  }
  Sized const sized{ traced, referrers, sizes };

  // Which Sections have a Payload, before anything is given a runtime address:
  // Place needs that much to keep them out of the Window, and needs nothing
  // else of storage — see docs/decisions/0017-payloads-and-banks.md.
  Storage storage = findPayloads( pruned, readOnly, sizes, sink );

  // Not const: what decodes while a `.xex` loads is placed into it after the
  // stored forms exist, by the load Step below.
  Layout layout = placeSections( sized, storage, readOnly, options.explain, sink );
  Placed const placed{ sized, layout };
  checkAssertions( placed, sink );
  clock.ended( "Place" );

  // After an error it is skipped, because Place refusing to place a Section is
  // not the same finding as a Layout that placed it wrong — see
  // docs/decisions/0005-test-strategy.md.
  if ( options.verifyLayout && !sink.hasErrors() )
  {
    verifyLayout( placed, storage, readOnly, sink );
    clock.ended( "verifying the layout" );
  }

  Bytes bytes = patch( placed, storage, sink );
  clock.ended( "Patch" );

  // The stored form of every Payload, then the Banks packed by what those came
  // to, and only then the Frames — which name where a Payload waits and so
  // could not be written before any of it. See
  // docs/decisions/0022-transforms.md and docs/decisions/0036-frames.md.
  transformPayloads( sized, storage, bytes );
  findHiddenAtLoad( placed, storage, bytes, sink );
  transformAtLoad( placed, storage, bytes, sink );
  placeLoad( placed, layout, storage, bytes, sink );
  // What every later Choice of a `choose` block changes, over the Layout the
  // load Step finished — see
  // docs/decisions/0302-a-document-may-be-chosen-while-a-xex-loads.md.
  if ( !later.empty() && !sink.hasErrors() )
  {
    storage.chooseAtLoad( findChoiceSites( sources, later, placed, storage, bytes, sink ) );
  }
  stageHiddenAtLoad( placed, storage, sink );
  placeStorage( placed, storage, sink );
  patchFrames( placed, storage, sink );
  patchColdFrame( placed, storage, bytes );
  clock.ended( "the transforms, the load and PlaceStorage" );
  if ( sink.hasErrors() )
  {
    return;
  }

  // Storage and the Bytes are finished, so the build is: what a Container is
  // written from.
  Patched const patched{ placed, storage, bytes };

  // The Project's `container` was chosen by the output file's name — see
  // docs/decisions/0018-xex-container.md.
  Emitted emitted;
  if ( project.container == Container::XEX )
  {
    emitted.bytes = emitXex( patched, sink ).bytes;
  }
  else if ( project.container == Container::ATR )
  {
    emitted.bytes = emitAtr( patched, sink ).bytes;
  }
  else if ( project.container == Container::CAR )
  {
    emitted.bytes = emitCar( patched, sink ).bytes;
  }
  else if ( project.container == Container::BS93 )
  {
    emitted.bytes = emitBs93( patched, sink ).bytes;
  }
  else if ( project.container == Container::LNX || project.container == Container::LYX )
  {
    emitted.bytes = emitLnx( patched, sink ).bytes;
  }
  else if ( project.container == Container::PRG )
  {
    emitted.bytes = emitPrg( patched, sink ).bytes;
  }
  else if ( CbmGeometry const* const disk = cbmGeometryOf( project.container ); disk != nullptr )
  {
    emitted.bytes = emitCbmDisk( patched, *disk, sink ).bytes;
  }
  else
  {
    RawImage image = emitRawImage( patched, sink );
    emitted.bytes = std::move( image.bytes );
    emitted.origin = image.origin;
  }
  clock.ended( "Emit" );
  // A writer that refuses says why. One that wrote nothing and said nothing
  // has met something an earlier Step was to rule out, and an empty file
  // written as though it were the program would be the worst way to show it.
  // A raw image of a program with no bytes is empty, and right to be.
  if ( !sink.hasErrors() && emitted.bytes.empty() && project.container != Container::RAW_IMAGE )
  {
    sink.add( diag::diagnostic( diag::DiagnosticId::CONTAINER_NOT_WRITTEN )
                  .arg( "container", std::string{ nameOf( project.container ) } ) );
  }
  if ( sink.hasErrors() )
  {
    return;
  }
  built( patched, emitted );
}

} // namespace

void build( diag::SourceManager& sources,
            Project& project,
            BuildOptions const& options,
            diag::DiagnosticSink& sink,
            std::function<void( Patched const&, Emitted const& )> const& built,
            std::function<void( Merged const& )> const& merged )
{
  StepClock clock;
  // Before Assemble, which fills the Project in as it runs.
  std::vector<Project> const later = laterChoicesOf( sources, project );
  std::vector<Module> modules = assembleProject( sources, project, sink );
  clock.ended( "Assemble" );
  buildFrom( sources, project, later, modules, options, sink, built, merged, clock );

  // Every Step that ran has reported: what a `.off` silences is known, and so
  // is a `.off` that silenced nothing.
  applySuppressions( sources, modules, sink );
}

Emitted buildWithFacts( diag::SourceManager& sources,
                        Project& project,
                        BuildOptions const& options,
                        diag::DiagnosticSink& sink,
                        std::set<FactSet> const& wanted,
                        bool map,
                        Facts& facts )
{
  bool const wantBytes = wanted.contains( FactSet::BYTES );
  bool const wantMap = map || wantBytes || wanted.contains( FactSet::MAP );
  bool const wantSymbols = wanted.contains( FactSet::SYMBOLS );
  Emitted emitted;
  build(
      sources,
      project,
      options,
      sink,
      [&]( Patched const& patched, Emitted const& written )
      {
        StepClock clock;
        emitted = written;
        // The map is read off the finished build, deciding nothing — see
        // docs/spec/memory-map.md.
        if ( wantMap )
        {
          facts.map = memoryMapOf( patched );
        }
        if ( facts.symbols.has_value() )
        {
          Resolved known{ patched.sizes(), patched.layout(), patched.target(), patched.modules() };
          placeSymbols( *facts.symbols, patched, known );
        }
        if ( wantBytes )
        {
          facts.bytes = containerFactsOf( patched, *facts.map, emitted.bytes, project.container );
        }
        if ( wantMap || wantSymbols )
        {
          clock.ended( "reading the map and the facts off the build" );
        }
      },
      [&]( Merged const& merged )
      {
        if ( wantSymbols )
        {
          facts.symbols = symbolsOf( merged );
        }
      } );
  return emitted;
}

} // namespace nga::model
