#include "nga/model/Storage.hpp"

#include "nga/model/Prune.hpp"
#include "nga/model/ReadOnly.hpp"
#include "nga/model/Slots.hpp"
#include "nga/model/Transition.hpp"

#include <algorithm>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace nga::model
{

Storage::Storage( std::span<Module const> modules )
{
  mByModule.resize( modules.size() );
  for ( std::size_t module = 0; module < modules.size(); ++module )
  {
    mByModule[module].resize( modules[module].sections().size() );
  }
}

void Storage::markPayload( SectionRef where )
{
  mByModule[where.module.value][where.section.value].payload = true;
}

void Storage::alignImage( SectionRef where )
{
  mByModule[where.module.value][where.section.value].aligned = true;
}

bool Storage::isImageAligned( SectionRef where ) const
{
  return at( where ).aligned;
}

void Storage::alignFrame( FrameIndex index )
{
  mFrames[index.value].aligned = true;
}

bool Storage::isFrameAligned( FrameIndex index ) const
{
  return mFrames[index.value].aligned;
}

void Storage::store( SectionRef where, std::uint8_t transform, std::vector<StoredPiece> pieces )
{
  Entry& entry = mByModule[where.module.value][where.section.value];
  entry.transform = transform;
  entry.pieces.clear();
  entry.pieces.reserve( pieces.size() );
  for ( StoredPiece& one : pieces )
  {
    entry.pieces.push_back(
        Piece{ .placed = false, .address = {}, .landing = one.landing, .form = std::move( one.form ) } );
  }
}

void Storage::live( SectionRef where, Residency live )
{
  mByModule[where.module.value][where.section.value].live = std::move( live );
}

Residency const& Storage::liveOf( SectionRef where ) const
{
  return at( where ).live;
}

void Storage::liveFrame( FrameIndex index, Residency live )
{
  mFrames[index.value].live = std::move( live );
}

Residency const& Storage::frameLiveOf( FrameIndex index ) const
{
  return mFrames[index.value].live;
}

void Storage::addPaneImage( SectionRef where, StorageAddress at )
{
  mPaneImages.push_back( PaneImage{ .where = where, .at = at } );
}

void Storage::place( SectionRef where, std::uint32_t piece, StorageAddress at )
{
  Piece& one = mByModule[where.module.value][where.section.value].pieces[piece];
  one.placed = true;
  one.address = at;
}

Storage::Entry const& Storage::at( SectionRef where ) const
{
  return mByModule[where.module.value][where.section.value];
}

bool Storage::hasPayload( SectionRef where ) const
{
  return at( where ).payload;
}

std::uint32_t Storage::pieceCountOf( SectionRef where ) const
{
  return static_cast<std::uint32_t>( at( where ).pieces.size() );
}

bool Storage::isPlaced( SectionRef where, std::uint32_t piece ) const
{
  Entry const& entry = at( where );
  return piece < entry.pieces.size() && entry.pieces[piece].placed;
}

// A Payload that no Step has stored a form for has no pieces at all, and these
// three answer for one as they answered before there were pieces: a Section
// that waits nowhere waits at nothing. Asking is what a caller does before it
// knows whether there is anything to ask about, and a partial answer here would
// make every one of them check first.
StorageAddress Storage::addressOf( SectionRef where, std::uint32_t piece ) const
{
  Entry const& entry = at( where );
  return piece < entry.pieces.size() ? entry.pieces[piece].address : StorageAddress{};
}

std::span<std::uint8_t const> Storage::formOf( SectionRef where, std::uint32_t piece ) const
{
  Entry const& entry = at( where );
  return piece < entry.pieces.size() ? std::span<std::uint8_t const>{ entry.pieces[piece].form }
                                     : std::span<std::uint8_t const>{};
}

std::uint32_t Storage::sizeOf( SectionRef where, std::uint32_t piece ) const
{
  Entry const& entry = at( where );
  return piece < entry.pieces.size() ? static_cast<std::uint32_t>( entry.pieces[piece].form.size() ) : 0;
}

std::uint8_t Storage::transformOf( SectionRef where ) const
{
  return at( where ).transform;
}

std::uint32_t Storage::landingOf( SectionRef where, std::uint32_t piece ) const
{
  Entry const& entry = at( where );
  return piece < entry.pieces.size() ? entry.pieces[piece].landing : 0;
}

void Storage::declareFrame( FrameIndex index,
                            std::optional<PhaseIndex> from,
                            PhaseIndex to,
                            std::vector<SectionRef> payloads,
                            std::uint32_t blocks,
                            std::uint32_t cellWrites,
                            std::uint32_t windows,
                            bool held )
{
  if ( mFrames.size() <= index.value )
  {
    mFrames.resize( index.value + 1 );
  }
  Frame& frame = mFrames[index.value];
  frame.declared = true;
  frame.from = from;
  frame.to = to;
  frame.payloads = std::move( payloads );
  frame.cellWrites = cellWrites;
  frame.size = sizeOfFrame( blocks, cellWrites, windows, held );
}

std::optional<FrameIndex> Storage::frameOf( std::optional<PhaseIndex> from, PhaseIndex to ) const
{
  for ( std::uint32_t index = 0; index < mFrames.size(); ++index )
  {
    if ( mFrames[index].declared && mFrames[index].from == from && mFrames[index].to == to )
    {
      return FrameIndex{ index };
    }
  }
  return std::nullopt;
}

std::span<SectionRef const> Storage::framePayloadsOf( FrameIndex index ) const
{
  return mFrames[index.value].payloads;
}

std::uint32_t Storage::frameCellWritesOf( FrameIndex index ) const
{
  return mFrames[index.value].cellWrites;
}

void Storage::placeFrame( FrameIndex index, StorageAddress at )
{
  mFrames[index.value].placed = true;
  mFrames[index.value].address = at;
}

void Storage::storeFrame( FrameIndex index, std::vector<std::uint8_t> bytes )
{
  mFrames[index.value].bytes = std::move( bytes );
}

std::uint32_t Storage::frameCount() const
{
  return static_cast<std::uint32_t>( mFrames.size() );
}

bool Storage::isFrameDeclared( FrameIndex index ) const
{
  return index.value < mFrames.size() && mFrames[index.value].declared;
}

bool Storage::isFramePlaced( FrameIndex index ) const
{
  return index.value < mFrames.size() && mFrames[index.value].placed;
}

StorageAddress Storage::frameAddressOf( FrameIndex index ) const
{
  return mFrames[index.value].address;
}

std::uint32_t Storage::frameSizeOf( FrameIndex index ) const
{
  return mFrames[index.value].size;
}

std::span<std::uint8_t const> Storage::frameBytesOf( FrameIndex index ) const
{
  return mFrames[index.value].bytes;
}

std::optional<PhaseIndex> Storage::frameFrom( FrameIndex index ) const
{
  return mFrames[index.value].from;
}

PhaseIndex Storage::frameTo( FrameIndex index ) const
{
  return mFrames[index.value].to;
}

/// Which Phases each Phase can reach by one edge or more.
std::vector<std::vector<bool>> reachabilityOf( PhaseGraph const& graph )
{
  std::size_t const count = graph.phases.size();
  std::vector<std::vector<bool>> reaches( count, std::vector<bool>( count, false ) );
  for ( std::uint32_t from = 0; from < count; ++from )
  {
    std::vector<PhaseIndex> pending( graph.phases[from].then.begin(), graph.phases[from].then.end() );
    while ( !pending.empty() )
    {
      PhaseIndex const at = pending.back();
      pending.pop_back();
      if ( reaches[from][at.value] )
      {
        continue;
      }
      reaches[from][at.value] = true;
      pending.insert( pending.end(), graph.phases[at.value].then.begin(), graph.phases[at.value].then.end() );
    }
  }
  return reaches;
}

Residency liveAcross( PhaseGraph const& graph, Residency const& needed )
{
  std::vector<std::vector<bool>> const reaches = reachabilityOf( graph );
  Residency live{ graph.phases.size() };
  for ( std::uint32_t phase = 0; phase < graph.phases.size(); ++phase )
  {
    bool still = needed.phaseCount() > phase && needed.includes( PhaseIndex{ phase } );
    for ( std::uint32_t ahead = 0; ahead < graph.phases.size() && !still; ++ahead )
    {
      still = reaches[phase][ahead] && needed.phaseCount() > ahead && needed.includes( PhaseIndex{ ahead } );
    }
    if ( still )
    {
      live.add( PhaseIndex{ phase } );
    }
  }
  return live;
}

/// A `root` Section holding no bytes, evicted between two Phases that need
/// it: what reaches a Root knows no Phases and finds whatever the Phase
/// between put there, and nothing copies it back, since there is nothing to
/// copy. An ordinary reservation is left alone — a gap is an eviction by
/// docs/decisions/0016-phases-and-residency.md, and the code that names it
/// initialises it on entry. One finding per Section, at the first gap in
/// Phase order.
void reportEvictedRoots( Pruned const& build, diag::DiagnosticSink& sink )
{
  PhaseGraph const& graph = build.phases();
  std::vector<std::vector<bool>> const reaches = reachabilityOf( graph );
  auto const nameOf = [&graph]( std::uint32_t phase ) { return graph.phases[phase].name.value_or( "(implicit)" ); };

  for ( std::uint32_t module = 0; module < build.modules().size(); ++module )
  {
    Module const& one = build.modules()[module];
    Residency const& residency = one.residency();
    for ( std::uint32_t index = 0; index < one.sections().size(); ++index )
    {
      Section const& section = one.sections()[index];
      SectionRef const where{ .module = ModuleIndex{ module }, .section = SectionIndex{ index } };
      if ( !section.isRoot() || section.emitsBytes() || !build.reachable().includes( where ) )
      {
        continue;
      }
      for ( std::uint32_t gap = 0; gap < graph.phases.size(); ++gap )
      {
        if ( residency.includes( PhaseIndex{ gap } ) )
        {
          continue;
        }
        std::optional<std::uint32_t> before;
        std::optional<std::uint32_t> after;
        for ( std::uint32_t phase = 0; phase < graph.phases.size(); ++phase )
        {
          if ( !residency.includes( PhaseIndex{ phase } ) )
          {
            continue;
          }
          if ( !before.has_value() && reaches[phase][gap] )
          {
            before = phase;
          }
          if ( !after.has_value() && reaches[gap][phase] )
          {
            after = phase;
          }
        }
        if ( !before.has_value() || !after.has_value() )
        {
          continue;
        }
        sink.add( diag::diagnostic( diag::DiagnosticId::ROOT_EVICTED )
                      .at( section.span().begin, section.span().length )
                      .arg( "section", one.displayNameOf( SectionIndex{ index }, build.sources() ) )
                      .arg( "gap", nameOf( gap ) )
                      .arg( "before", nameOf( *before ) )
                      .arg( "after", nameOf( *after ) ) );
        break;
      }
    }
  }
}

bool takesColdStart( Project const& project )
{
  // Storage is where a Frame waits, so a board with no banks takes no cold
  // start: there is nothing for one to wait in, and a program on such a board
  // holds every byte it has in ROM.
  return project.container == Container::CAR && project.target.unitCount > 0;
}

/// The first position at or after `at` where `size` bytes fit inside one unit,
/// within the part of it a program may use. What moves a position on is either
/// of two things: a driver promised `span none`, which may not be handed an
/// image that crosses a unit, and a unit that is not usable to its end, which a
/// diskette of zoned tracks has. Both are answered the same way, since an image
/// that may not cross cannot lie in the gap either.
/// The ceiling one stored form is held to, which is the widest unit there is
/// where a driver was promised that nothing crosses one, and nothing at all
/// otherwise.
std::uint32_t storedCeilingOf( Target const& target, std::optional<Driver> const& driver )
{
  if ( !driver.has_value() || !driver->spansNothing || target.bankSize() == 0 )
  {
    return 0;
  }
  std::uint32_t widest = 0;
  for ( std::uint32_t unit = 0; unit * target.bankSize() < target.storageSize(); ++unit )
  {
    widest = std::max( widest, target.usableInUnit( unit ) );
  }
  return widest;
}

std::uint32_t blocksForPayload( std::uint32_t storedCeiling, std::uint32_t extent )
{
  // Room enough for the whole, or a ceiling that holds nothing: one block.
  if ( storedCeiling <= FORM_HEADER_SIZE || extent + FORM_HEADER_SIZE <= storedCeiling )
  {
    return 1;
  }
  // Each piece carries its own two bytes of size, so what one holds of the
  // input is that much less — and **less again**, because a format may come out
  // longer than it went in and a piece that did would carry fewer of its bytes
  // than the division assumes. An eighth is far more than any of the three
  // needs: a run-length code that meets nothing to run pays one byte in every
  // hundred and twenty-eight, and ZX0 less. What the margin costs is a block of
  // a Frame that is never used, six bytes, on a Payload that had to be cut at
  // all; what it buys is that the count cannot outrun the room.
  //
  // It has to be a bound and not the answer because the Frame is placed before
  // the Transform Step runs: a `.transition` carries the address its Frame
  // waits at, so Patch writes that address before any form exists. Sizing it
  // exactly would mean placing Frames after Transform and patching those bytes
  // later, which is in the queue.
  std::uint32_t const perPiece = ( storedCeiling - FORM_HEADER_SIZE ) * 7 / 8;
  if ( perPiece == 0 )
  {
    return 1;
  }
  return ( ( extent + perPiece - 1 ) / perPiece ) + 1;
}

std::uint32_t fitInUnit( Target const& target, std::uint32_t at, std::uint32_t size )
{
  std::uint32_t const stride = target.bankSize();
  if ( stride == 0 || size == 0 )
  {
    return at;
  }
  while ( at < target.storageSize() )
  {
    std::uint32_t const unit = at / stride;
    if ( ( at % stride ) + size <= target.usableInUnit( unit ) )
    {
      return at;
    }
    at = ( unit + 1 ) * stride;
  }
  return at;
}

Storage findPayloads( Pruned const& build, ReadOnly const& readOnly, Sizes const& sizes, diag::DiagnosticSink& sink )
{
  // What the medium does about an offset, which decides whether `fast` on an
  // edge is worth any storage at all; and what fills the silence where a
  // statement says nothing, which is the Intent's — `speed` asks for the quick
  // edge, while `size` and `fit` pack tight, alignment being a choice that can
  // push storage past its size and that the tool could not give back. See
  // docs/decisions/0177-intent.md and 0221.
  bool const seeksForward = build.project().driver.has_value() && build.project().driver->seeksForward;
  bool const quickBySilence = build.project().intent == Intent::SPEED;
  PhaseGraph const& graph = build.phases();
  Storage storage{ build.modules() };
  reportEvictedRoots( build, sink );

  // The load set of every edge, which is the whole of what decides that a
  // Section waits somewhere — nothing declares it. The Phases whose edges
  // load a Section are what it is needed in, and it is live in those and in
  // every Phase that reaches one.
  std::vector<std::vector<Residency>> needed( build.modules().size() );
  for ( std::uint32_t module = 0; module < build.modules().size(); ++module )
  {
    needed[module].assign( build.modules()[module].sections().size(), Residency{ graph.phases.size() } );
  }
  // The cold start is an edge into the entry Phase from nowhere, which a
  // Container with no loader takes to put the entry Phase's own bytes where
  // they belong — see
  // docs/decisions/0216-a-car-names-its-format-and-the-cold-start-is-an-edge.md.
  // Its Payloads are marked with every other edge's, since a Payload belongs
  // to a Section and not to an edge.
  std::vector<SectionRef> coldBlocks;
  if ( takesColdStart( build.project() ) )
  {
    coldBlocks = loadSetOf( graph, std::nullopt, graph.entry, build.modules() );
    std::erase_if( coldBlocks,
                   [&readOnly, &build]( SectionRef const& where )
                   { return readOnly.standsInRom( where ) || !build.reachable().includes( where ); } );
    for ( SectionRef const where : coldBlocks )
    {
      storage.markPayload( where );
      needed[where.module.value][where.section.value].add( graph.entry );
    }
  }

  // What a list of Payloads comes to in blocks. A Payload is one unless no unit
  // of storage holds the whole of it — the Transform Step has not run and no
  // form exists yet, so this is what the lengths allow rather than what the
  // encoding will come to, and a Frame sized for a block that is never used
  // leaves it unread.
  std::uint32_t const ceiling = storedCeilingOf( build.project().target, build.project().driver );
  auto const blocksFor = [&sizes, ceiling]( std::vector<SectionRef> const& payloads )
  {
    std::uint32_t blocks = 0;
    for ( SectionRef const where : payloads )
    {
      std::uint32_t const extent = sizes.isKnown( where ) ? sizes.initialisedExtentOf( where ).size() : 0;
      blocks += blocksForPayload( ceiling, extent );
    }
    return blocks;
  };

  bool const coldStart = takesColdStart( build.project() );
  for ( std::uint32_t from = 0; from < graph.phases.size(); ++from )
  {
    for ( PhaseIndex const next : graph.phases[from].then )
    {
      for ( SectionRef const where : loadSetOf( graph, PhaseIndex{ from }, next, build.modules() ) )
      {
        // What Prune dropped is loaded by nothing, and neither is what stands
        // in ROM: the bytes are in the address space already, and a Transition
        // that tried to copy them would be writing to ROM — see
        // docs/decisions/0215-a-cartridge-is-rom-and-a-section-stands-in-it-when-nothing-writes-it.md.
        if ( build.reachable().includes( where ) && !readOnly.standsInRom( where ) )
        {
          storage.markPayload( where );
          needed[where.module.value][where.section.value].add( PhaseIndex{ from } );
        }
      }
    }
  }
  for ( std::uint32_t module = 0; module < build.modules().size(); ++module )
  {
    for ( std::uint32_t index = 0; index < needed[module].size(); ++index )
    {
      SectionRef const where{ .module = ModuleIndex{ module }, .section = SectionIndex{ index } };
      if ( storage.hasPayload( where ) )
      {
        storage.live( where, liveAcross( graph, needed[module][index] ) );
      }
    }
  }

  // Every edge some reachable `.transition` takes has a Frame: for each
  // statement, one per Phase its Section is present in, since at run time the
  // code does not know which one it is in. Numbered as they are met, which is
  // Project order; sized by what the edge may load and write, which the end
  // of Assemble settled; and placed now, first fit from the start of the
  // Banks, because the statement's bytes name where its Frames wait and Patch
  // writes those before any Payload's stored form exists.
  auto const slotCount = static_cast<std::uint32_t>( slotsOf( build.modules() ).size() );
  std::uint32_t frames = 0;
  if ( coldStart )
  {
    storage.declareFrame( FrameIndex{ frames },
                          std::nullopt,
                          graph.entry,
                          coldBlocks,
                          blocksFor( coldBlocks ),
                          slotCount,
                          static_cast<std::uint32_t>( baseOrderOf( build.project() ).size() ),
                          build.project().framesHeld );
    // Read before anything of the program runs, and on every reset for as long
    // as it lives, so it is live wherever the program is.
    storage.liveFrame( FrameIndex{ frames }, Residency::all( graph.phases.size() ) );
    ++frames;
  }
  for ( std::uint32_t module = 0; module < build.modules().size(); ++module )
  {
    Module const& one = build.modules()[module];
    for ( std::uint32_t index = 0; index < one.sections().size(); ++index )
    {
      SectionRef const where{ .module = ModuleIndex{ module }, .section = SectionIndex{ index } };
      if ( !build.reachable().includes( where ) )
      {
        continue;
      }
      for ( Chunk const& chunk : one.sections()[index].chunks() )
      {
        auto const* const transition = std::get_if<TransitionContent>( &chunk.content );
        if ( transition == nullptr || !transition->target.has_value() )
        {
          continue;
        }
        PhaseIndex const to = transition->target.value_or( PhaseIndex{} );

        // An edge that must be quick wants every image it opens to begin where
        // a unit does — and only where the driver reaches an offset by reading
        // up to it, since alignment costs storage and buys nothing on a medium
        // an offset is free on. Where two statements take one edge and either
        // says `fast`, the edge is fast: the cost is paid once and the quick
        // one is the one that asked. See
        // docs/decisions/0221-an-edge-that-must-be-quick.md.
        bool const quick = ( transition->fast || quickBySilence ) && seeksForward;
        for ( std::uint32_t phase = 0; phase < graph.phases.size(); ++phase )
        {
          PhaseIndex const from{ phase };
          std::vector<PhaseIndex> const& then = graph.phases[phase].then;
          if ( !one.residency().includes( from ) || std::ranges::find( then, to ) == then.end() )
          {
            continue;
          }
          if ( std::optional<FrameIndex> const already = storage.frameOf( from, to ); already.has_value() )
          {
            if ( quick )
            {
              storage.alignFrame( *already );
              for ( SectionRef const& block : storage.framePayloadsOf( *already ) )
              {
                storage.alignImage( block );
              }
            }
            continue;
          }
          // The same set, less what stands in ROM: a Frame's blocks are what
          // the routine copies, and it copies none of those.
          std::vector<SectionRef> blocks = loadSetOf( graph, from, to, build.modules() );
          std::erase_if( blocks, [&readOnly]( SectionRef const& where ) { return readOnly.standsInRom( where ); } );
          if ( quick )
          {
            for ( SectionRef const& block : blocks )
            {
              storage.alignImage( block );
            }
          }
          storage.declareFrame( FrameIndex{ frames },
                                from,
                                to,
                                blocks,
                                blocksFor( blocks ),
                                slotCount,
                                static_cast<std::uint32_t>( baseOrderOf( build.project() ).size() ),
                                build.project().framesHeld );
          // After the Frame exists and not before: a Frame is declared into a
          // vector this grows.
          if ( quick )
          {
            storage.alignFrame( FrameIndex{ frames } );
          }
          // Read when the edge is taken, so needed in the Phase left and
          // live in it and in every Phase that reaches it.
          Residency neededBy{ graph.phases.size() };
          neededBy.add( from );
          storage.liveFrame( FrameIndex{ frames }, liveAcross( graph, neededBy ) );
          ++frames;
        }
      }
    }
  }

  // Storage is one space end to end — an image may run from one unit into
  // the next, since the driver's stream carries on — so the Frames are
  // placed one after the other from the start of it.
  Target const& target = build.target();
  bool const confined = build.project().driver.has_value() && build.project().driver->spansNothing;
  std::uint32_t used = 0;
  for ( std::uint32_t index = 0; index < storage.frameCount(); ++index )
  {
    FrameIndex const frame{ index };
    std::uint32_t const size = storage.frameSizeOf( frame );
    // A Frame of a quick edge begins where a unit does, like the images it
    // names: the routine opens it once per block, at a position that grows, so
    // the offsets it is opened at are the ones that must cost nothing.
    if ( storage.isFrameAligned( frame ) && target.bankSize() > 0 )
    {
      used = ( ( used + target.bankSize() - 1 ) / target.bankSize() ) * target.bankSize();
    }
    // A Frame that may not cross a unit, or would land in a part of one that
    // is not there, begins at the next unit instead.
    if ( confined || !target.unitUsable.empty() )
    {
      used = fitInUnit( target, used, size );
    }
    if ( used + size <= target.storageSize() )
    {
      storage.placeFrame( frame, target.addressAt( used ) );
      used += size;
    }
  }

  return storage;
}

void placeStorage( Placed const& build, Storage& storage, diag::DiagnosticSink& sink )
{
  diag::SourceManager const& sources = build.sources();
  // A driver promised `span none` is held to it here: nothing it is handed
  // crosses from one unit into the next, so it needs no way of carrying there.
  bool const confined = build.project().driver.has_value() && build.project().driver->spansNothing;
  Target const& target = build.target();
  Layout const& layout = build.layout();
  Sizes const& sizes = build.sizes();

  // Bytes of storage that are taken: for good, by what is written at load —
  // the Frames, a Pane's Section with bytes, and every image placed so far —
  // or in the Phases a reservation in a Pane is present, which an image may
  // lie under only while it is dead. Positions end to end, as an image may
  // run from one unit into the next.
  struct Taken
  {
    std::uint32_t begin = 0;
    std::uint32_t end = 0;

    /// Nothing for bytes taken in every Phase.
    std::optional<Residency> present;
  };

  std::vector<Taken> taken;
  for ( std::uint32_t index = 0; index < storage.frameCount(); ++index )
  {
    FrameIndex const frame{ index };
    if ( storage.isFramePlaced( frame ) )
    {
      std::uint32_t const begin = target.positionOf( storage.frameAddressOf( frame ) );
      taken.push_back( Taken{ .begin = begin, .end = begin + storage.frameSizeOf( frame ), .present = std::nullopt } );
    }
  }

  // A Pane's Section with bytes waits in its Bank at its own offset within
  // the Window, written by the Container; one pinned to a named state has
  // no Bank a loader fills. A Pane's reservation takes the same bytes of
  // the same Bank — of every member's, for a family — while it is present.
  bool noBanksReported = false;
  for ( std::uint32_t module = 0; module < build.modules().size(); ++module )
  {
    Module const& one = build.modules()[module];
    for ( std::uint32_t index = 0; index < one.sections().size(); ++index )
    {
      Section const& section = one.sections()[index];
      SectionRef const where{ .module = ModuleIndex{ module }, .section = SectionIndex{ index } };
      if ( !section.pane().has_value() || !build.reachable().includes( where ) || !layout.isPlaced( where ) ||
           !sizes.isKnown( where ) || sizes.sizeOfSection( where ) == 0 )
      {
        continue;
      }
      Pane const& pane = target.panes[section.pane()->value];
      Window const& window = target.windows[pane.window.value];
      std::optional<UnitSetIndex> const set = target.unitSetShownBy( pane.window );
      std::optional<std::uint32_t> const state = layout.stateOfPane( *section.pane() );
      if ( pane.state.has_value() || !set.has_value() || !state.has_value() )
      {
        if ( pane.state.has_value() && section.emitsBytes() )
        {
          sink.add( diag::diagnostic( diag::DiagnosticId::PANE_STATE_PAYLOAD )
                        .at( section.span().begin, section.span().length )
                        .arg( "section", one.displayNameOf( where.section, sources ) )
                        .arg( "pane", pane.name ) );
        }
        continue;
      }
      // The offset within the Window: the ranges laid end to end, which is
      // what an address in a Bank means whichever Window shows it.
      auto const offsetOf = [&window]( std::uint32_t address )
      {
        std::uint32_t offset = 0;
        for ( AddressRange const& range : window.ranges )
        {
          if ( address >= range.begin && address < range.end )
          {
            return offset + ( address - range.begin );
          }
          offset += range.size();
        }
        return offset;
      };
      std::uint32_t const first = window.firstStateOf( *set, target.unitSets ).value_or( 0 );
      std::uint32_t const bank = *state - first;
      if ( section.emitsBytes() )
      {
        std::uint32_t const address = layout.addressOf( where ) + sizes.initialisedExtentOf( where ).begin;
        // One image per member, as the reservation below takes one run per
        // member. A family's Sections stand in every member at one offset so
        // that code shown any member finds them there, and a Container fills
        // each Bank on its own, so each needs its own copy of the bytes.
        for ( std::uint32_t member = bank; member < bank + pane.count && member < target.unitCount; ++member )
        {
          storage.addPaneImage( where, StorageAddress{ .bank = BankIndex{ member }, .offset = offsetOf( address ) } );
        }
      }
      // Only the storage's own set has positions an image could take.
      if ( target.storageUnits != set )
      {
        continue;
      }
      std::uint32_t const offset = offsetOf( layout.addressOf( where ) );
      for ( std::uint32_t member = bank; member < bank + pane.count && member < target.unitCount; ++member )
      {
        std::uint32_t const begin = ( member * target.unitSize ) + offset;
        taken.push_back( Taken{ .begin = begin,
                                .end = begin + sizes.sizeOfSection( where ),
                                .present = section.emitsBytes() ? std::nullopt : std::optional{ one.residency() } } );
      }
    }
  }

  // The order the images are placed in: **edge by edge**, and within an edge
  // the order the routine loads them, then whatever no edge loads in Project
  // order. It costs nothing and duplicates nothing — an image two edges share
  // lands once, where the first of them claims it — and what it buys is that
  // every `open` after the first is a forward skip, which on a cartridge is
  // the page already in and on a diskette the sector already read. It buys
  // that only where the Project holds an edge's descriptors, so it is done
  // only there: without them the routine returns to the Frame between blocks,
  // the next image is opened from the Frame's position whatever order they
  // stand in, and a program that gained nothing would have its storage laid
  // out differently for it. See
  // docs/decisions/0226-an-edge-holds-its-descriptors-in-the-block-that-loads-last.md.
  std::vector<SectionRef> order;
  auto const consider = [&order]( SectionRef where )
  {
    if ( std::ranges::find( order, where ) == order.end() )
    {
      order.push_back( where );
    }
  };
  for ( std::uint32_t index = 0; build.project().framesHeld && index < storage.frameCount(); ++index )
  {
    if ( storage.isFrameDeclared( FrameIndex{ index } ) )
    {
      for ( EdgeBlock const& block : edgeBlocksOf( FrameIndex{ index }, build, storage ) )
      {
        consider( block.payload );
      }
    }
  }
  for ( std::uint32_t module = 0; module < build.modules().size(); ++module )
  {
    for ( std::uint32_t index = 0; index < build.modules()[module].sections().size(); ++index )
    {
      consider( SectionRef{ .module = ModuleIndex{ module }, .section = SectionIndex{ index } } );
    }
  }

  // Each Payload then takes the first run of bytes free for it, which is what
  // keeps one run's storage the same as the next one's — except that the
  // images an edge which must be quick opens go **first**, each rounded up to
  // a unit, and the rest then fill what they left. That order is the whole of
  // the alignment: a first fit fills holes already. See
  // docs/decisions/0221-an-edge-that-must-be-quick.md.
  for ( int pass = 0; pass < 2; ++pass )
  {
    bool const aligning = pass == 0;
    for ( SectionRef const where : order )
    {
      Module const& one = build.modules()[where.module.value];
      Section const& section = one.sections()[where.section.value];
      if ( storage.isImageAligned( where ) != aligning )
      {
        continue;
      }

      if ( !storage.hasPayload( where ) )
      {
        continue;
      }

      // A Payload is one form unless no unit held the whole of it, and then
      // it is several, each placed on its own — they are independent forms,
      // so nothing asks them to be near one another.
      for ( std::uint32_t piece = 0; piece < storage.pieceCountOf( where ); ++piece )
      {
        // What the Payload occupies in storage: the length of the stored form,
        // which the Transform Step recorded from the bytes Patch produced. A
        // Section an earlier Step could not size has no form and nothing to
        // wait for.
        std::uint32_t const size = storage.sizeOf( where, piece );
        if ( size == 0 )
        {
          continue;
        }
        std::string const name = one.displayNameOf( where.section, sources );

        if ( target.unitCount == 0 )
        {
          sink.add( diag::diagnostic( diag::DiagnosticId::NO_STORAGE )
                        .at( section.span().begin, section.span().length )
                        .arg( "section", name )
                        .arg( "size", size )
                        .note( diag::diagnostic( diag::DiagnosticId::NO_BANKS ) ) );
          noBanksReported = true;
          continue;
        }

        // The bytes this image may not lie on: everything taken for good, and
        // every reservation present in a Phase the image is live in.
        Residency const& live = storage.liveOf( where );
        std::vector<AddressRange> blocked;
        for ( Taken const& held : taken )
        {
          if ( !held.present.has_value() || held.present->intersects( live ) )
          {
            blocked.push_back( AddressRange{ .begin = held.begin, .end = held.end } );
          }
        }
        std::ranges::sort( blocked, []( AddressRange const& a, AddressRange const& b ) { return a.begin < b.begin; } );
        std::uint32_t const unit = aligning && target.bankSize() > 0 ? target.bankSize() : 1;
        auto const upTo = [unit]( std::uint32_t at ) { return ( ( at + unit - 1 ) / unit ) * unit; };

        // A driver promised that nothing crosses a unit is held to it: an
        // image that would straddle begins at the next unit instead. That
        // moves the candidate forward past blocks already weighed, so the
        // scan runs again until it stops moving.
        bool const inOneUnit = confined || !target.unitUsable.empty();
        auto const confine = [&target, inOneUnit, size]( std::uint32_t at )
        { return inOneUnit ? fitInUnit( target, at, size ) : at; };

        // No unit holds it at all, which is a different fault from a full
        // medium: the widest unit is the ceiling and the author has to say so.
        std::uint32_t widest = 0;
        if ( inOneUnit )
        {
          for ( std::uint32_t which = 0; which * target.bankSize() < target.storageSize(); ++which )
          {
            widest = std::max( widest, target.usableInUnit( which ) );
          }
        }
        if ( inOneUnit && size > widest )
        {
          sink.add( diag::diagnostic( diag::DiagnosticId::IMAGE_SPANS_A_UNIT )
                        .at( section.span().begin, section.span().length )
                        .arg( "section", name )
                        .arg( "size", size )
                        .arg( "available", widest ) );
          continue;
        }

        std::uint32_t candidate = confine( 0 );
        for ( bool moved = true; moved; )
        {
          moved = false;
          for ( AddressRange const& block : blocked )
          {
            if ( candidate + size <= block.begin )
            {
              break;
            }
            std::uint32_t const next = confine( upTo( std::max( candidate, block.end ) ) );
            if ( next != candidate )
            {
              candidate = next;
              moved = true;
            }
          }
        }
        if ( candidate + size > target.storageSize() )
        {
          sink.add( diag::diagnostic( diag::DiagnosticId::NO_STORAGE )
                        .at( section.span().begin, section.span().length )
                        .arg( "section", name )
                        .arg( "size", size ) );
          continue;
        }
        storage.place( where, piece, target.addressAt( candidate ) );
        taken.push_back( Taken{ .begin = candidate, .end = candidate + size, .present = std::nullopt } );
      }
    }
  }

  // A Frame the first half could not place: no Bank at all, or none with
  // room for it. A Frame has no source of its own, so a finding about one
  // names the edge and sorts by it; no Bank at all is one finding, and where
  // a Payload has already said it, the Frames say nothing more.
  for ( std::uint32_t index = 0; index < storage.frameCount(); ++index )
  {
    FrameIndex const frame{ index };
    if ( !storage.isFrameDeclared( frame ) || storage.isFramePlaced( frame ) )
    {
      continue;
    }
    std::uint32_t const size = storage.frameSizeOf( frame );
    std::string const name = nameOfFrame( build.phases(), storage.frameFrom( frame ), storage.frameTo( frame ) );
    if ( target.unitCount == 0 )
    {
      if ( !noBanksReported )
      {
        sink.add( diag::diagnostic( diag::DiagnosticId::NO_STORAGE )
                      .arg( "section", name )
                      .arg( "size", size )
                      .note( diag::diagnostic( diag::DiagnosticId::NO_BANKS ) )
                      .sortedBy( name ) );
        noBanksReported = true;
      }
      continue;
    }
    sink.add( diag::diagnostic( diag::DiagnosticId::NO_STORAGE )
                  .arg( "section", name )
                  .arg( "size", size )
                  .sortedBy( name ) );
  }
}

} // namespace nga::model
