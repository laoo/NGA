#include "nga/model/Pipeline.hpp"

#include "nga/model/Assemble.hpp"
#include "nga/model/Emit.hpp"
#include "nga/model/Expand.hpp"
#include "nga/model/Merge.hpp"
#include "nga/model/Patch.hpp"
#include "nga/model/Place.hpp"
#include "nga/model/Prune.hpp"
#include "nga/model/Size.hpp"
#include "nga/model/Storage.hpp"
#include "nga/model/Suppressions.hpp"
#include "nga/model/Trace.hpp"
#include "nga/model/Transform.hpp"
#include "nga/model/TypeCheck.hpp"
#include "nga/model/VerifyLayout.hpp"

#include <span>

namespace nga::model
{

namespace
{

/// Merge through Emit over the assembled Modules. Errors stop the build at the
/// end of the Step that raised them, so that a run reports everything that
/// Step found rather than the first thing.
void buildFrom( diag::SourceManager const& sources,
                Project const& project,
                std::span<Module> modules,
                BuildOptions const& options,
                diag::DiagnosticSink& sink,
                std::function<void( Patched const&, Emitted const& )> const& built )
{
  GlobalSymbols const symbols = merge( modules, sink );

  // Merge's second result. A table cannot be built while a Module is
  // assembled, and Size needs one before it runs, which is what fixes this
  // position exactly — see docs/decisions/0013-charset-declaration.md.
  Charsets const charsets = resolveCharsets( sources, symbols, sink );

  // What `.if` decides and what a macro use expands to, before anything walks
  // a Chunk: a Reference in a branch the build excluded would otherwise hold
  // its Section against Prune, and the type check would report on code that
  // is not there — see docs/decisions/0049-recursive-macros.md. The gate
  // stands after it so that a use naming no macro is reported in the same
  // run as a duplicate export.
  expand( sources, symbols, charsets, project, modules, sink );
  if ( sink.hasErrors() )
  {
    return;
  }

  // What `.root` hands the hardware is a Root from here on — see
  // docs/decisions/0061-root-at-the-taking.md.
  markRoots( sources, symbols, modules, sink );

  // What every Step after Merge is handed: the results produced so far, and
  // nothing of the ones that do not exist yet — see
  // docs/decisions/0032-a-step-is-handed-the-build-so-far.md.
  Merged const merged{ sources, project, symbols, charsets };

  // What the Roots reach. Before the type check, so that a Reference from a
  // Section nothing reaches holds no Movable Section still; the rules are
  // still applied to every Chunk — see docs/decisions/0033-prune.md.
  Reachable const reachable = prune( merged, sink );
  Pruned const pruned{ merged, reachable };

  // Which Temporaries are never live at once, over the same References; what
  // Place reads to let them share an address — see
  // docs/decisions/0034-trace.md.
  Interference const interference = trace( pruned, sink );
  Traced const traced{ pruned, interference };

  Freezes const freezes = checkTypes( pruned, sink );
  Sizes const sizes = computeSizes( pruned, sink );
  if ( sink.hasErrors() )
  {
    return;
  }
  Sized const sized{ traced, freezes, sizes };

  // Which Sections have a Payload, before anything is given a runtime address:
  // Place needs that much to keep them out of the Window, and needs nothing
  // else of storage — see docs/decisions/0017-payloads-and-banks.md.
  Storage storage = findPayloads( pruned, sink );

  Layout const layout = placeSections( sized, storage, options.explain, sink );
  Placed const placed{ sized, layout };
  checkAssertions( placed, sink );

  // After an error it is skipped, because Place refusing to place a Section is
  // not the same finding as a Layout that placed it wrong — see
  // docs/decisions/0005-test-strategy.md.
  if ( options.verifyLayout && !sink.hasErrors() )
  {
    verifyLayout( placed, storage, sink );
  }

  Bytes bytes = patch( placed, storage, sink );

  // The stored form of every Payload, then the Banks packed by what those came
  // to, and only then the Frames — which name where a Payload waits and so
  // could not be written before any of it. See
  // docs/decisions/0022-transforms.md and docs/decisions/0036-frames.md.
  transformPayloads( sized, storage, bytes );
  placeStorage( placed, storage, sink );
  patchFrames( placed, storage, sink );
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
  else
  {
    RawImage image = emitRawImage( patched, sink );
    emitted.bytes = std::move( image.bytes );
    emitted.origin = image.origin;
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
            std::function<void( Patched const&, Emitted const& )> const& built )
{
  std::vector<Module> modules = assembleProject( sources, project, sink );
  buildFrom( sources, project, modules, options, sink, built );

  // Every Step that ran has reported: what a `.off` silences is known, and so
  // is a `.off` that silenced nothing.
  applySuppressions( sources, modules, sink );
}

} // namespace nga::model
